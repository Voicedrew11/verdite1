// A room: up to ROOM_PEERS WebSockets of one build, relaying signals between them. One Durable
// Object per (game, room); hibernates between frames, so each socket's state is its attachment.
import { DurableObject } from "cloudflare:workers";
import type { Env } from "./index";
import { issue, revoke } from "./turn";
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
  welcomed: boolean; // has had its welcome; only welcomed peers are seen by the others
  turnUser: string | null;
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

function refuse(ws: WebSocket, code: ErrorCode, message: string): void {
  send(ws, { type: "error", code, message });
  ws.close(1008, code);
}

function newPeerId(): string {
  const bytes = crypto.getRandomValues(new Uint8Array(8));
  return Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("");
}

export class Room extends DurableObject<Env> {
  // Lost when the object hibernates, which only forgives a burst.
  private windows = new WeakMap<WebSocket, Window>();

  private peers(): { ws: WebSocket; peer: Peer }[] {
    return this.ctx
      .getWebSockets()
      .filter((ws) => ws.readyState === WebSocket.OPEN)
      .map((ws) => ({ ws, peer: ws.deserializeAttachment() as Peer | null }))
      .filter((entry): entry is { ws: WebSocket; peer: Peer } => entry.peer !== null);
  }

  async fetch(request: Request): Promise<Response> {
    const build = new URL(request.url).searchParams.get("build") ?? "";
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

    // Accepted before the credential is fetched, so a second join meanwhile counts this one.
    const ids = new Set(present.map(({ peer }) => peer.peer));
    let id = newPeerId();
    while (ids.has(id)) id = newPeerId();
    const self: Peer = { peer: id, build, welcomed: false, turnUser: null };
    this.ctx.acceptWebSocket(server);
    server.serializeAttachment(self);
    this.ctx.waitUntil(this.welcome(server, self));
    return new Response(null, { status: 101, webSocket: client });
  }

  private async welcome(ws: WebSocket, self: Peer): Promise<void> {
    let credential;
    try {
      credential = await issue(this.env);
    } catch (error) {
      console.error(String(error));
      refuse(ws, "unavailable", "no TURN credentials");
      return;
    }
    if (ws.readyState !== WebSocket.OPEN) {
      if (credential.username) await revoke(this.env, credential.username);
      return;
    }
    // From here to the end runs without a pause, so each pair of peers is introduced once: the
    // one welcomed later finds the other in its welcome and offers.
    const others = this.peers().filter(({ ws: other, peer }) => other !== ws && peer.welcomed);
    self.welcomed = true;
    self.turnUser = credential.username;
    ws.serializeAttachment(self);
    send(ws, {
      type: "welcome",
      protocol: PROTOCOL,
      peer: self.peer,
      peers: others.map(({ peer }) => peer.peer),
      iceServers: credential.iceServers,
    });
    for (const { ws: other } of others) send(other, { type: "join", peer: self.peer });
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
    const self = ws.deserializeAttachment() as Peer;
    if (this.overRate(ws)) {
      send(ws, { type: "error", code: "rate_limited", message: `over ${FRAMES_PER_SECOND} frames a second` });
      return;
    }
    if (typeof message !== "string" || message.length > FRAME_BYTES) {
      send(ws, { type: "error", code: "bad_message", message: "frames are JSON text up to 64 KiB" });
      return;
    }
    let frame: unknown;
    try {
      frame = JSON.parse(message);
    } catch {
      send(ws, { type: "error", code: "bad_message", message: "not JSON" });
      return;
    }
    if (!self.welcomed || typeof frame !== "object" || frame === null || (frame as { type?: unknown }).type !== "signal") {
      send(ws, { type: "error", code: "bad_message", message: "expected a signal" });
      return;
    }
    const { to, data } = frame as { to?: unknown; data?: unknown };
    const target = this.peers().find(({ peer }) => peer.welcomed && peer.peer === to && peer.peer !== self.peer);
    if (!target) {
      send(ws, { type: "error", code: "unknown_peer", message: `no peer ${String(to)} in the room` });
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
    const self = ws.deserializeAttachment() as Peer | null;
    if (!self?.welcomed) return;
    self.welcomed = false; // a close after an error leaves only once
    try {
      ws.serializeAttachment(self);
    } catch {
      // closed: its attachment is gone with it
    }
    for (const { ws: other, peer } of this.peers())
      if (other !== ws && peer.welcomed) send(other, { type: "leave", peer: self.peer });
    if (self.turnUser) await revoke(this.env, self.turnUser);
  }
}
