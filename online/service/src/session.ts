// What a room and a floor's mesh share: the frames, the sockets' attachments, the frame limit and
// a peer's TURN credentials (online/protocol/PROTOCOL.md). Holds no state of its own.
import type { Env } from "./index";
import { addressKey } from "./quota";
import { credentialSeconds, issue, revoke } from "./turn";
import {
  FRAME_BYTES,
  FRAMES_PER_SECOND,
  PING_FRAME,
  PONG_FRAME,
  SILENT_SECONDS,
  type ErrorCode,
  type ServerFrame,
} from "./wire";

const TURN_CREDENTIALS_KEPT = 2;

// The address a request counts as for the per-address limits and quotas.
export function clientAddress(request: Request): string {
  return addressKey(request.headers.get("CF-Connecting-IP") ?? "local");
}

// A socket's address, for the per-address limits.
export interface Client {
  ip: string;
}

// A peer of a room or a mesh: its id and its live TURN credentials, newest last. Its connections
// move to a new credential only after it arrives, so the one before stays until the next.
export interface Member extends Client {
  peer: string;
  turnUsers: string[];
}

interface Window {
  start_ms: number;
  frames: number;
}

export function send(ws: WebSocket, frame: ServerFrame): void {
  try {
    ws.send(JSON.stringify(frame));
  } catch {
    // closing: its close event cleans up
  }
}

export function fail(ws: WebSocket, code: ErrorCode, message: string): void {
  send(ws, { type: "error", code, message });
}

export function refuse(ws: WebSocket, code: ErrorCode, message: string): void {
  fail(ws, code, message);
  ws.close(1008, code);
}

// A peer id not in taken: 16 random hex digits.
export function freshPeerId(taken: Set<string>): string {
  for (;;) {
    const bytes = crypto.getRandomValues(new Uint8Array(8));
    const id = Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("");
    if (!taken.has(id)) return id;
  }
}

export function attachment<T>(ws: WebSocket): T | null {
  try {
    return ws.deserializeAttachment() as T | null;
  } catch {
    return null;
  }
}

export function store<T>(ws: WebSocket, value: T | null): void {
  try {
    ws.serializeAttachment(value);
  } catch {
    // closed: its attachment is gone with it
  }
}

// The open sockets of a tag (all when none) that have an attachment, with it.
export function members<T>(ctx: DurableObjectState, tag?: string): { ws: WebSocket; member: T }[] {
  return ctx
    .getWebSockets(tag)
    .filter((ws) => ws.readyState === WebSocket.OPEN)
    .map((ws) => ({ ws, member: attachment<T>(ws) }))
    .filter((entry): entry is { ws: WebSocket; member: T } => entry.member !== null);
}

// Liveness (PROTOCOL.md, "Staying in a room"): a client pings every 10 seconds, answered at the
// edge without waking the object, so a crashed or frozen game, or one whose network went, still has
// an open socket but stops pinging. A socket that never pinged is an older client's and is kept.

export function answerPings(ctx: DurableObjectState): void {
  ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair(PING_FRAME, PONG_FRAME));
}

// SILENT_SECONDS, or the env's (3 to 45) to test dropping.
export function silentSeconds(env: Env): number {
  const seconds = Number(env.SILENT_SECONDS);
  return Number.isInteger(seconds) && seconds >= 3 && seconds <= SILENT_SECONDS ? seconds : SILENT_SECONDS;
}

// How often an object with members looks for silent ones.
export function livenessEveryMs(env: Env): number {
  return (silentSeconds(env) * 1000) / 3;
}

// Whether a member of a tag (all when none) pings, so the object has someone to watch.
export function anyPinging(ctx: DurableObjectState, tag?: string): boolean {
  return members(ctx, tag).some(({ ws }) => ctx.getWebSocketAutoResponseTimestamp(ws) !== null);
}

// The members of a tag (all when none) that pinged once and then went silent.
export function silentMembers<T>(ctx: DurableObjectState, env: Env, tag?: string): { ws: WebSocket; member: T }[] {
  const now_ms = Date.now();
  const silent_ms = silentSeconds(env) * 1000;
  return members<T>(ctx, tag).filter(({ ws }) => {
    const pinged = ctx.getWebSocketAutoResponseTimestamp(ws);
    return pinged !== null && now_ms - pinged.getTime() > silent_ms;
  });
}

// Closes a silent member's socket, after the caller has made it leave as its close would have.
export function closeSilent(ws: WebSocket): void {
  try {
    ws.close(4000, "silent");
  } catch {
    // already closed
  }
}

// Sets the object's next liveness alarm unless one is due sooner. Set on a join, since the
// joiner's first ping comes after its welcome; the alarm sets the next only while anyPinging.
export async function watchLiveness(ctx: DurableObjectState, env: Env): Promise<void> {
  const due_ms = Date.now() + livenessEveryMs(env);
  const alarm_ms = await ctx.storage.getAlarm();
  if (alarm_ms === null || alarm_ms > due_ms) await ctx.storage.setAlarm(due_ms);
}

// The frames a socket sent in its current second, at most FRAMES_PER_SECOND. Lost when the object
// hibernates, which only forgives a burst.
export class FrameRate {
  private windows = new WeakMap<WebSocket, Window>();

  over(ws: WebSocket): boolean {
    const now_ms = Date.now();
    let window = this.windows.get(ws);
    if (!window || now_ms - window.start_ms >= 1000) {
      window = { start_ms: now_ms, frames: 0 };
      this.windows.set(ws, window);
    }
    window.frames += 1;
    return window.frames > FRAMES_PER_SECOND;
  }
}

// A frame's JSON object, or null after an error has gone to the socket. A frame that is not an
// object comes back as {}, so its type is unknown to the caller.
export function readFrame(ws: WebSocket, rate: FrameRate, message: string | ArrayBuffer): Record<string, unknown> | null {
  if (rate.over(ws)) {
    fail(ws, "rate_limited", `over ${FRAMES_PER_SECOND} frames a second`);
    return null;
  }
  if (typeof message !== "string" || message.length > FRAME_BYTES) {
    fail(ws, "bad_message", "frames are JSON text up to 64 KiB");
    return null;
  }
  let frame: unknown;
  try {
    frame = JSON.parse(message);
  } catch {
    fail(ws, "bad_message", "not JSON");
    return null;
  }
  return typeof frame === "object" && frame !== null ? (frame as Record<string, unknown>) : {};
}

// A TURN credential for a peer with someone to connect to (connectable says how many), within its
// address's limit and the month's budget. The peer keeps its last two: a third revokes the oldest.
export async function grantTurn(env: Env, ws: WebSocket, self: Member, connectable: number): Promise<void> {
  if (connectable === 0) {
    fail(ws, "alone", "no other peer to connect to");
    return;
  }
  if (!(await env.TURN_LIMIT.limit({ key: self.ip })).success) {
    fail(ws, "rate_limited", "too many TURN credentials from this address");
    return;
  }
  const budget = env.BUDGET.get(env.BUDGET.idFromName("turn"));
  const verdict = await budget.verdict(self.ip, (self.turnUsers ?? []).length > 0);
  if (verdict !== "ok") {
    fail(ws, "unavailable", verdict === "address" ? "too much TURN traffic from this address" : "the service is over its TURN budget");
    return;
  }
  let credential;
  try {
    credential = await issue(env);
  } catch (error) {
    console.error(String(error));
    fail(ws, "unavailable", "no TURN credentials");
    return;
  }
  if (credential.username) await budget.issued(self.ip, credential.username, Date.now() + credentialSeconds(env) * 1000);
  const latest = attachment<Member>(ws);
  if (ws.readyState !== WebSocket.OPEN || !latest) {
    if (credential.username) await budget.ended(self.ip, [credential.username]);
    if (credential.username) await revoke(env, credential.username);
    return;
  }
  const live = credential.username ? [...latest.turnUsers, credential.username] : latest.turnUsers;
  const retired = live.slice(0, -TURN_CREDENTIALS_KEPT);
  store(ws, { ...latest, turnUsers: live.slice(-TURN_CREDENTIALS_KEPT) });
  send(ws, { type: "turn", iceServers: credential.iceServers, seconds: credentialSeconds(env) });
  for (const username of retired) await revoke(env, username);
  if (retired.length) await budget.ended(self.ip, retired);
}

// Revokes a peer's TURN credentials when it leaves.
export async function revokeTurn(env: Env, member: Member): Promise<void> {
  const usernames = member.turnUsers ?? [];
  for (const username of usernames) await revoke(env, username);
  if (usernames.length) await env.BUDGET.get(env.BUDGET.idFromName("turn")).ended(member.ip, usernames);
}
