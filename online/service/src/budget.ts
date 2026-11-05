// The TURN budget: the service's own cap, since Cloudflare has none. A scheduled check each minute
// reads the TURN key's egress (what Cloudflare bills) from the GraphQL analytics API, for the month
// and for each credential today, and records it here; rooms and meshes ask before issuing a
// credential. Credentials can only be limited after the fact (src/quota.ts): an address over its
// quota loses them all and is refused for a day, a day over its share stops new connections until
// tomorrow, the month's budget stops new connections, and the hard cap ends everything.
//
// Without CF_ACCOUNT_ID and CF_ANALYTICS_TOKEN there is no reading and no cap. A failed reading
// keeps the last one, so a cap reached stays reached until a reading says otherwise.
import { DurableObject } from "cloudflare:workers";
import type { Env } from "./index";
import { ADDRESS_BLOCK_MS, REVOKES_PER_RUN, judgeUsage, turnVerdict, type TurnVerdict } from "./quota";
import { revoke } from "./turn";

const GRAPHQL = "https://api.cloudflare.com/client/v4/graphql";
const BYTES_PER_GB = 1e9;
const KEPT_AFTER_END_MS = 2 * 24 * 60 * 60 * 1000; // a credential still counts for its address after it ends

const MONTH_QUERY = `query TurnEgress($accountTag: string!, $keyId: string!, $from: Date!, $to: Date!) {
  viewer {
    accounts(filter: { accountTag: $accountTag }) {
      callsTurnUsageAdaptiveGroups(limit: 1, filter: { keyId: $keyId, date_geq: $from, date_leq: $to }) {
        sum { egressBytes }
      }
    }
  }
}`;

// The heaviest users come first, so a limit past what is used still holds every abuser.
const CREDENTIALS_QUERY = `query TurnByCredential($accountTag: string!, $keyId: string!, $day: Date!) {
  viewer {
    accounts(filter: { accountTag: $accountTag }) {
      callsTurnUsageAdaptiveGroups(limit: 1000, filter: { keyId: $keyId, date_geq: $day, date_leq: $day }, orderBy: [sum_egressBytes_DESC]) {
        dimensions { username }
        sum { egressBytes }
      }
    }
  }
}`;

interface Reading {
  month: string; // "2026-10"
  egress_bytes: number;
  today_bytes: number;
  read_ms: number;
}

interface Issued {
  ends_ms: number;
  revoked: boolean;
}

interface DayTotals {
  day: string;
  read_ms: number;
  totals: Record<string, number>;
}

function budgetBytes(env: Env): number {
  const gb = Number(env.TURN_BUDGET_GB);
  return (Number.isFinite(gb) && gb > 0 ? gb : 500) * BYTES_PER_GB;
}

function hardBytes(env: Env): number {
  const gb = Number(env.TURN_HARD_GB);
  return (Number.isFinite(gb) && gb > 0 ? gb : 900) * BYTES_PER_GB;
}

function credentialKey(address: string, username: string): string {
  return `cred:${address}:${username}`;
}

// A credential key's parts: the address is hex, and a username could hold anything.
function keyParts(key: string): { address: string; username: string } {
  const after_address = key.indexOf(":", "cred:".length);
  return { address: key.slice("cred:".length, after_address), username: key.slice(after_address + 1) };
}

// Minutes since the last totals, or since the day began: what they may have grown by.
function minutesSince(before: DayTotals | undefined, day: string, now_ms: number): number {
  const since_ms = before?.day === day ? before.read_ms : Date.parse(`${day}T00:00:00Z`);
  return (now_ms - since_ms) / 60000;
}

export class Budget extends DurableObject<Env> {
  // An address as it is kept: hashed with a salt that never leaves this object.
  private async addressId(ip: string): Promise<string> {
    let salt = await this.ctx.storage.get<string>("salt");
    if (!salt) {
      salt = crypto.randomUUID();
      await this.ctx.storage.put("salt", salt);
    }
    const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(`${salt}|${ip}`));
    return Array.from(new Uint8Array(digest).slice(0, 16), (b) => b.toString(16).padStart(2, "0")).join("");
  }

  private async currentUsage(now: Date): Promise<{ month_bytes: number; today_bytes: number }> {
    const reading = await this.ctx.storage.get<Reading>("reading");
    if (!reading || reading.month !== now.toISOString().slice(0, 7)) return { month_bytes: 0, today_bytes: 0 }; // a new month starts within budget
    return { month_bytes: reading.egress_bytes, today_bytes: reading.today_bytes ?? 0 };
  }

  async verdict(ip: string, renewing: boolean): Promise<TurnVerdict> {
    const now = new Date();
    const address = await this.addressId(ip);
    const blocked_until = (await this.ctx.storage.get<number>(`block:${address}`)) ?? 0;
    const held = await this.ctx.storage.list<Issued>({ prefix: `cred:${address}:` });
    const live = [...held.values()].filter((issued) => !issued.revoked && issued.ends_ms > now.getTime()).length;
    return turnVerdict({
      ...(await this.currentUsage(now)),
      blocked: blocked_until > now.getTime(),
      live_credentials: live,
      renewing,
      budget_bytes: budgetBytes(this.env),
      hard_bytes: hardBytes(this.env),
      now,
    });
  }

  async issued(ip: string, username: string, ends_ms: number): Promise<void> {
    await this.ctx.storage.put<Issued>(credentialKey(await this.addressId(ip), username), { ends_ms, revoked: false });
  }

  // Revoked by a room or a mesh: no longer live, but what it used still counts for its address.
  async ended(ip: string, usernames: string[]): Promise<void> {
    const address = await this.addressId(ip);
    for (const username of usernames) {
      const key = credentialKey(address, username);
      const issued = await this.ctx.storage.get<Issued>(key);
      if (issued) await this.ctx.storage.put<Issued>(key, { ...issued, revoked: true });
    }
  }

  async record(reading: Reading): Promise<void> {
    await this.ctx.storage.put("reading", reading);
  }

  async reading(): Promise<(Reading & { budget_bytes: number; hard_bytes: number }) | null> {
    const reading = await this.ctx.storage.get<Reading>("reading");
    return reading ? { ...reading, budget_bytes: budgetBytes(this.env), hard_bytes: hardBytes(this.env) } : null;
  }

  // What each credential used today (null when it could not be read): addresses over a quota lose
  // theirs and are refused, and past the hard cap every live credential goes, the heaviest first.
  async judge(read: [string, number][] | null, month_bytes: number): Promise<void> {
    const used = read ?? [];
    const now = Date.now();
    const day = new Date(now).toISOString().slice(0, 10);
    const held = await this.ctx.storage.list<Issued>({ prefix: "cred:" });
    const owners = new Map<string, string>();
    for (const [key, issued] of held) {
      const { address, username } = keyParts(key);
      if (issued.ends_ms + KEPT_AFTER_END_MS < now) await this.ctx.storage.delete(key);
      else owners.set(username, address);
    }
    const blocks = await this.ctx.storage.list<number>({ prefix: "block:" });
    const ended_blocks = [...blocks].filter(([, until_ms]) => until_ms <= now).map(([key]) => key);
    if (ended_blocks.length) await this.ctx.storage.delete(ended_blocks.slice(0, 128));
    const before = await this.ctx.storage.get<DayTotals>("day_totals");
    let over: string[] = [];
    if (read) {
      const judged = judgeUsage(owners, new Map(used), before?.day === day ? before.totals : {}, minutesSince(before, day, now));
      over = judged.over;
      await this.ctx.storage.put<DayTotals>("day_totals", { day, read_ms: now, totals: judged.totals });
    }
    for (const address of over) await this.ctx.storage.put(`block:${address}`, now + ADDRESS_BLOCK_MS);
    if (over.length) console.error(`budget: ${over.length} addresses over a quota were blocked`);

    const hard = month_bytes >= hardBytes(this.env);
    if (hard) console.error("budget: past the hard cap, ending every credential");
    const by_use = new Map(used);
    const due: { key: string; username: string; issued: Issued }[] = [];
    for (const [key, issued] of held) {
      const { address, username } = keyParts(key);
      if (!issued.revoked && issued.ends_ms > now && (hard || over.includes(address))) due.push({ key, username, issued });
    }
    due.sort((a, b) => (by_use.get(b.username) ?? 0) - (by_use.get(a.username) ?? 0));
    for (const { key, username, issued } of due.slice(0, REVOKES_PER_RUN)) {
      await revoke(this.env, username);
      await this.ctx.storage.put<Issued>(key, { ...issued, revoked: true });
    }
    // Credentials from before this object kept them: nothing says when they end, so they go once.
    for (const address of over.filter((each) => each.startsWith("unknown:")).slice(0, REVOKES_PER_RUN)) {
      await revoke(this.env, address.slice("unknown:".length));
    }
  }
}

type Groups = { dimensions?: { username?: string }; sum?: { egressBytes?: number } }[];

async function query(env: Env, text: string, variables: Record<string, string>): Promise<Groups | null> {
  const response = await fetch(GRAPHQL, {
    method: "POST",
    headers: { Authorization: `Bearer ${env.CF_ANALYTICS_TOKEN}`, "Content-Type": "application/json" },
    body: JSON.stringify({ query: text, variables }),
  });
  const body = (await response.json().catch(() => null)) as {
    data?: { viewer?: { accounts?: { callsTurnUsageAdaptiveGroups?: Groups }[] } };
    errors?: { message: string }[] | null;
  } | null;
  if (!response.ok || !body || body.errors?.length) {
    console.error(`budget: analytics HTTP ${response.status}: ${body?.errors?.map((e) => e.message).join("; ") ?? "no body"}`);
    return null;
  }
  return body.data?.viewer?.accounts?.[0]?.callsTurnUsageAdaptiveGroups ?? [];
}

// The scheduled check: reads the month's egress and each credential's today, and acts on them.
export async function checkBudget(env: Env): Promise<void> {
  if (!env.CF_ACCOUNT_ID || !env.CF_ANALYTICS_TOKEN || !env.TURN_KEY_ID) {
    console.log("budget: no analytics token or account id, no cap");
    return;
  }
  const now = new Date();
  const month = now.toISOString().slice(0, 7);
  const day = now.toISOString().slice(0, 10);
  const account = { accountTag: env.CF_ACCOUNT_ID, keyId: env.TURN_KEY_ID };
  const groups = await query(env, MONTH_QUERY, { ...account, from: `${month}-01`, to: day });
  if (!groups) return;
  const egress_bytes = groups.reduce((total, group) => total + (group.sum?.egressBytes ?? 0), 0);
  const budget = env.BUDGET.get(env.BUDGET.idFromName("turn"));
  const by_credential = await query(env, CREDENTIALS_QUERY, { ...account, day });
  const used: [string, number][] | null =
    by_credential &&
    by_credential.flatMap((group) => {
      const username = group.dimensions?.username;
      return username ? [[username, group.sum?.egressBytes ?? 0] as [string, number]] : [];
    });
  // Unread, today's stays as it was (0 once it is another day).
  const last = await budget.reading();
  const today_bytes = used
    ? used.reduce((total, [, bytes]) => total + bytes, 0)
    : last?.read_ms && new Date(last.read_ms).toISOString().slice(0, 10) === day
      ? last.today_bytes
      : 0;
  await budget.record({ month, egress_bytes, today_bytes, read_ms: Date.now() });
  await budget.judge(used, egress_bytes);
  console.log(`budget: ${(egress_bytes / BYTES_PER_GB).toFixed(3)} GB this month, ${(today_bytes / BYTES_PER_GB).toFixed(3)} GB today`);
}
