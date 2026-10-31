// What a room and a floor's mesh share: the frames, the sockets' attachments, the frame limit and
// a peer's TURN credentials (online/protocol/PROTOCOL.md). Holds no state of its own.
import type { Env } from "./index";
import { credentialSeconds, issue, revoke } from "./turn";
import { FRAME_BYTES, FRAMES_PER_SECOND, type ErrorCode, type ServerFrame } from "./wire";

const TURN_CREDENTIALS_KEPT = 2;

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
  if (!(await env.BUDGET.get(env.BUDGET.idFromName("turn")).allowed())) {
    fail(ws, "unavailable", "the service is over its TURN budget for the month");
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
  const latest = attachment<Member>(ws);
  if (ws.readyState !== WebSocket.OPEN || !latest) {
    if (credential.username) await revoke(env, credential.username);
    return;
  }
  const live = credential.username ? [...latest.turnUsers, credential.username] : latest.turnUsers;
  const retired = live.slice(0, -TURN_CREDENTIALS_KEPT);
  store(ws, { ...latest, turnUsers: live.slice(-TURN_CREDENTIALS_KEPT) });
  send(ws, { type: "turn", iceServers: credential.iceServers, seconds: credentialSeconds(env) });
  for (const username of retired) await revoke(env, username);
}

// Revokes a peer's TURN credentials when it leaves.
export async function revokeTurn(env: Env, member: Member): Promise<void> {
  for (const username of member.turnUsers ?? []) await revoke(env, username);
}
