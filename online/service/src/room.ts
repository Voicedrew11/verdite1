// A room: up to ROOM_PEERS WebSockets of one build, relaying signals between them and handing out
// TURN credentials to peers that have someone to connect to. One Durable Object per (game, room);
// hibernates between frames, so each socket's state is its attachment.
import { DurableObject } from "cloudflare:workers";
import type { Env } from "./index";
import { issue, revoke, CREDENTIAL_SECONDS } from "./turn";
import {
  FRAME_BYTES,
  FRAMES_PER_SECOND,
  PROTOCOL,
  ROOM_PEERS,
  type ErrorCode,
  type ServerFrame,
} from "./wire";

interface Peer {
  peer: string;
  build: string;
  ip: string; // for the per-address credential limit
  turnUser: string | null; // the live TURN credential, revoked when replaced or on leaving
}

interface Window {
  start_ms: number;
  frames: number;
}

function send(ws: WebSocket, frame: ServerFrame): void {
  try {
    ws.send(JSON.stringify(frame));
  } catch {
    // closing: its close event cleans up
  }
}

function fail(ws: WebSocket, code: ErrorCode, message: string): void {
  send(ws, { type: "error", code, message });
}

function refuse(ws: WebSocket, code: ErrorCode, message: string): void {
  fail(ws, code, message);
  ws.close(1008, code);
}

function newPeerId(): string {
  const bytes = crypto.getRandomValues(new Uint8Array(8));
  return Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("");
}

function attachment(ws: WebSocket): Peer | null {
  try {
    return ws.deserializeAttachment() as Peer | null;
  } catch {
    return null;
  }
}

function store(ws: WebSocket, peer: Peer): void {
  try {
    ws.serializeAttachment(peer);
  } catch {
    // closed: its attachment is gone with it
  }
}

export class Room extends DurableObject<Env> {
  // Lost when the object hibernates, which only forgives a burst.
  private windows = new WeakMap<WebSocket, Window>();

  private peers(): { ws: WebSocket; peer: Peer }[] {
    return this.ctx
      .getWebSockets()
      .filter((ws) => ws.readyState === WebSocket.OPEN)
      .map((ws) => ({ ws, peer: attachment(ws) }))
      .filter((entry): entry is { ws: WebSocket; peer: Peer } => entry.peer !== null);
  }

  async fetch(request: Request): Promise<Response> {
    const build = new URL(request.url).searchParams.get("build") ?? "";
    const ip = request.headers.get("CF-Connecting-IP") ?? "local";
    const [client, server] = Object.values(new WebSocketPair()) as [WebSocket, WebSocket];
    const present = this.peers();

    if (present.length >= ROOM_PEERS) {
      this.ctx.acceptWebSocket(server); // no attachment: never a peer
      refuse(server, "full", `the room has ${ROOM_PEERS} peers`);
      return new Response(null, { status: 101, webSocket: client });
    }
    const roomBuild = present[0]?.peer.build;
    if (roomBuild !== undefined && roomBuild !== build) {
      this.ctx.acceptWebSocket(server); // no attachment: never a peer
      refuse(server, "build_mismatch", `the room's peers have build ${roomBuild}`);
      return new Response(null, { status: 101, webSocket: client });
    }

    // Nothing here waits, so each pair of peers is introduced once: the later one finds the
    // other in its welcome and offers.
    const ids = new Set(present.map(({ peer }) => peer.peer));
    let id = newPeerId();
    while (ids.has(id)) id = newPeerId();
    this.ctx.acceptWebSocket(server);
    store(server, { peer: id, build, ip, turnUser: null });
    send(server, { type: "welcome", protocol: PROTOCOL, peer: id, peers: [...ids] });
    for (const { ws: other } of present) send(other, { type: "join", peer: id });
    return new Response(null, { status: 101, webSocket: client });
  }

  // A credential only for a peer with someone to connect to, within its address's limit and
  // the month's budget; a new one replaces (and revokes) the peer's last.
  private async turn(ws: WebSocket, self: Peer): Promise<void> {
    if (!this.peers().some(({ ws: other }) => other !== ws)) {
      fail(ws, "alone", "no other peer to connect to");
      return;
    }
    if (!(await this.env.TURN_LIMIT.limit({ key: self.ip })).success) {
      fail(ws, "rate_limited", "too many TURN credentials from this address");
      return;
    }
    if (!(await this.env.BUDGET.get(this.env.BUDGET.idFromName("turn")).allowed())) {
      fail(ws, "unavailable", "the service is over its TURN budget for the month");
      return;
    }
    let credential;
    try {
      credential = await issue(this.env);
    } catch (error) {
      console.error(String(error));
      fail(ws, "unavailable", "no TURN credentials");
      return;
    }
    const latest = attachment(ws);
    if (ws.readyState !== WebSocket.OPEN || !latest) {
      if (credential.username) await revoke(this.env, credential.username);
      return;
    }
    const replaced = latest.turnUser;
    store(ws, { ...latest, turnUser: credential.username });
    send(ws, { type: "turn", iceServers: credential.iceServers, seconds: CREDENTIAL_SECONDS });
    if (replaced) await revoke(this.env, replaced);
  }

  private overRate(ws: WebSocket): boolean {
    const now_ms = Date.now();
    let window = this.windows.get(ws);
    if (!window || now_ms - window.start_ms >= 1000) {
      window = { start_ms: now_ms, frames: 0 };
      this.windows.set(ws, window);
    }
    window.frames += 1;
    return window.frames > FRAMES_PER_SECOND;
  }

  async webSocketMessage(ws: WebSocket, message: string | ArrayBuffer): Promise<void> {
    const self = attachment(ws);
    if (!self) return; // a refused socket on its way out
    if (this.overRate(ws)) {
      fail(ws, "rate_limited", `over ${FRAMES_PER_SECOND} frames a second`);
      return;
    }
    if (typeof message !== "string" || message.length > FRAME_BYTES) {
      fail(ws, "bad_message", "frames are JSON text up to 64 KiB");
      return;
    }
    let frame: unknown;
    try {
      frame = JSON.parse(message);
    } catch {
      fail(ws, "bad_message", "not JSON");
      return;
    }
    const type = typeof frame === "object" && frame !== null ? (frame as { type?: unknown }).type : undefined;
    if (type === "turn") {
      await this.turn(ws, self);
      return;
    }
    if (type !== "signal") {
      fail(ws, "bad_message", "expected a signal or a turn request");
      return;
    }
    const { to, data } = frame as { to?: unknown; data?: unknown };
    const target = this.peers().find(({ peer }) => peer.peer === to && peer.peer !== self.peer);
    if (!target) {
      fail(ws, "unknown_peer", `no peer ${String(to)} in the room`);
      return;
    }
    send(target.ws, { type: "signal", from: self.peer, data });
  }

  async webSocketClose(ws: WebSocket, code: number): Promise<void> {
    await this.leave(ws);
    try {
      ws.close(code === 1005 ? 1000 : code, "bye");
    } catch {
      // already closed
    }
  }

  async webSocketError(ws: WebSocket): Promise<void> {
    await this.leave(ws);
  }

  private async leave(ws: WebSocket): Promise<void> {
    const self = attachment(ws);
    if (!self) return;
    try {
      ws.serializeAttachment(null); // a close after an error leaves only once
    } catch {
      // closed
    }
    for (const { ws: other } of this.peers())
      if (other !== ws) send(other, { type: "leave", peer: self.peer });
    if (self.turnUser) await revoke(this.env, self.turnUser);
  }
}
