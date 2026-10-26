// The month's TURN budget: the service's own cap, since Cloudflare has none. A scheduled check
// reads the TURN key's egress (what Cloudflare bills) for the month so far from the GraphQL
// analytics API and records it here; rooms ask allowed() before issuing a credential.
//
// Without CF_ACCOUNT_ID and CF_ANALYTICS_TOKEN there is no reading and no cap. A failed reading
// keeps the last one, so a cap reached stays reached until a reading says otherwise.
import { DurableObject } from "cloudflare:workers";
import type { Env } from "./index";

const GRAPHQL = "https://api.cloudflare.com/client/v4/graphql";
const BYTES_PER_GB = 1e9;

const QUERY = `query TurnEgress($accountTag: string!, $keyId: string!, $from: Date!, $to: Date!) {
  viewer {
    accounts(filter: { accountTag: $accountTag }) {
      callsTurnUsageAdaptiveGroups(limit: 1, filter: { keyId: $keyId, date_geq: $from, date_leq: $to }) {
        sum { egressBytes }
      }
    }
  }
}`;

interface Reading {
  month: string; // "2026-10"
  egress_bytes: number;
  read_ms: number;
}

function budgetBytes(env: Env): number {
  const gb = Number(env.TURN_BUDGET_GB);
  return (Number.isFinite(gb) && gb > 0 ? gb : 500) * BYTES_PER_GB;
}

export class Budget extends DurableObject<Env> {
  async allowed(): Promise<boolean> {
    const reading = await this.ctx.storage.get<Reading>("reading");
    const month = new Date().toISOString().slice(0, 7);
    if (!reading || reading.month !== month) return true; // a new month starts within budget
    return reading.egress_bytes < budgetBytes(this.env);
  }

  async record(reading: Reading): Promise<void> {
    await this.ctx.storage.put("reading", reading);
  }

  async reading(): Promise<(Reading & { budget_bytes: number }) | null> {
    const reading = await this.ctx.storage.get<Reading>("reading");
    return reading ? { ...reading, budget_bytes: budgetBytes(this.env) } : null;
  }
}

// The scheduled check: reads the month's egress and records it.
export async function checkBudget(env: Env): Promise<void> {
  if (!env.CF_ACCOUNT_ID || !env.CF_ANALYTICS_TOKEN || !env.TURN_KEY_ID) {
    console.log("budget: no analytics token or account id, no cap");
    return;
  }
  const now = new Date();
  const month = now.toISOString().slice(0, 7);
  const response = await fetch(GRAPHQL, {
    method: "POST",
    headers: { Authorization: `Bearer ${env.CF_ANALYTICS_TOKEN}`, "Content-Type": "application/json" },
    body: JSON.stringify({
      query: QUERY,
      variables: {
        accountTag: env.CF_ACCOUNT_ID,
        keyId: env.TURN_KEY_ID,
        from: `${month}-01`,
        to: now.toISOString().slice(0, 10),
      },
    }),
  });
  const body = (await response.json().catch(() => null)) as {
    data?: { viewer?: { accounts?: { callsTurnUsageAdaptiveGroups?: { sum?: { egressBytes?: number } }[] }[] } };
    errors?: { message: string }[] | null;
  } | null;
  if (!response.ok || !body || body.errors?.length) {
    console.error(`budget: analytics HTTP ${response.status}: ${body?.errors?.map((e) => e.message).join("; ") ?? "no body"}`);
    return;
  }
  const groups = body.data?.viewer?.accounts?.[0]?.callsTurnUsageAdaptiveGroups ?? [];
  const egress_bytes = groups.reduce((total, group) => total + (group.sum?.egressBytes ?? 0), 0);
  await env.BUDGET.get(env.BUDGET.idFromName("turn")).record({ month, egress_bytes, read_ms: Date.now() });
  console.log(`budget: ${(egress_bytes / BYTES_PER_GB).toFixed(3)} GB of ${budgetBytes(env) / BYTES_PER_GB} GB this month`);
}
