// How much TURN traffic an address and a day may take: the decisions alone, with no I/O, so they can
// be tested without a service (test/quota.mjs). A TURN credential is good for any traffic to any
// public address until it ends, and Cloudflare bills what its edge sends the client, so a credential
// can only be limited by watching what it used and ending it.

const BYTES_PER_MB = 1e6;
const BYTES_PER_GB = 1e9;

// A player receives at most about 0.1 MB/s (three others' poses and a floor's frames at 30 updates a
// second), usually far less, and a house or a phone network may share an address.
export const ADDRESS_MINUTE_BYTES = 100 * BYTES_PER_MB; // past this in a minute: not players
export const ADDRESS_DAY_BYTES = 10 * BYTES_PER_GB;
export const ADDRESS_LIVE_CREDENTIALS = 32; // two each for a room and a floor's mesh, for several players
export const ADDRESS_BLOCK_MS = 7 * 24 * 60 * 60 * 1000; // a day's would let one address spoil every day
export const PACE_FACTOR = 3; // a day may take this many times the even share of what is left of the month
export const REVOKES_PER_RUN = 40; // a scheduled run's subrequests are limited; the rest go next minute

// What rate limits and quotas count: an IPv4 address, or an IPv6 address's /64, which is what one
// customer is given. Counting whole IPv6 addresses would give one machine 2^64 of every limit.
export function addressKey(ip: string): string {
  if (!ip.includes(":")) return ip;
  const [head = "", tail] = ip.toLowerCase().split("%")[0]!.split("::");
  const front = head ? head.split(":") : [];
  const back = tail ? tail.split(":") : [];
  const mapped = back.at(-1)?.includes(".") ?? front.at(-1)?.includes(".") ?? false;
  if (mapped) return (back.at(-1) ?? front.at(-1)) as string; // ::ffff:1.2.3.4 is 1.2.3.4
  const missing = tail === undefined ? 0 : 8 - front.length - back.length;
  const groups = [...front, ...Array<string>(missing).fill("0"), ...back];
  return `${groups.slice(0, 4).map((group) => group.replace(/^0+(?=.)/, "")).join(":")}::/64`;
}

// What today may take: the month's budget left, shared over the days left, times PACE_FACTOR. So
// someone burning it stops new connections until tomorrow, and never for the rest of the month.
export function dayAllowanceBytes(budget_bytes: number, month_bytes: number, today_bytes: number, now: Date): number {
  const days = new Date(Date.UTC(now.getUTCFullYear(), now.getUTCMonth() + 1, 0)).getUTCDate();
  const days_left = days - now.getUTCDate() + 1;
  const before_today = Math.max(0, month_bytes - today_bytes);
  return (Math.max(0, budget_bytes - before_today) / days_left) * PACE_FACTOR;
}

export interface TurnUsage {
  month_bytes: number;
  today_bytes: number;
}

export type TurnVerdict = "ok" | "address" | "budget";

export interface VerdictAsked extends TurnUsage {
  blocked: boolean; // the address was caught over a quota
  live_credentials: number; // the address's
  renewing: boolean; // the peer holds a credential: its connections end without a new one
  budget_bytes: number;
  hard_bytes: number;
  now: Date;
}

// Whether a peer gets a credential. Renewals pass what only stops new connections, so a game in
// progress goes on while the budget is short; past the hard cap nothing does.
export function turnVerdict(asked: VerdictAsked): TurnVerdict {
  if (asked.blocked) return "address";
  if (asked.month_bytes >= asked.hard_bytes) return "budget";
  if (asked.renewing) return "ok";
  if (asked.live_credentials >= ADDRESS_LIVE_CREDENTIALS) return "address";
  if (asked.month_bytes >= asked.budget_bytes) return "budget";
  const allowance = dayAllowanceBytes(asked.budget_bytes, asked.month_bytes, asked.today_bytes, asked.now);
  return asked.today_bytes >= allowance ? "budget" : "ok";
}

// Each address's bytes today, from each credential's, and those over a quota: over the day's, or
// over the minute's rate since `previous`, read `minutes` ago. A credential we did not issue, or no
// longer know, counts as an address of its own.
export function judgeUsage(
  owners: ReadonlyMap<string, string>, // credential to address
  used: ReadonlyMap<string, number>, // credential to bytes today
  previous: Readonly<Record<string, number>>,
  minutes: number,
): { totals: Record<string, number>; over: string[] } {
  const totals: Record<string, number> = {};
  for (const [username, bytes] of used) {
    const address = owners.get(username) ?? `unknown:${username}`;
    totals[address] = (totals[address] ?? 0) + bytes;
  }
  const over = Object.entries(totals)
    .filter(([address, bytes]) => bytes >= ADDRESS_DAY_BYTES || bytes - (previous[address] ?? 0) >= ADDRESS_MINUTE_BYTES * Math.max(1, minutes))
    .map(([address]) => address);
  return { totals, over };
}
