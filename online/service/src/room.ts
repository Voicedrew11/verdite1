// A room: up to ROOM_PEERS WebSockets of one build, relaying signals between them and handing out
// TURN credentials to peers that have someone to connect to. One Durable Object per (game, room);
// hibernates between frames, so each socket's state is its attachment.
import { DurableObject } from "cloudflare:workers";
import type { Env } from "./index";
import {
  attachment,
  fail,
  freshPeerId,
  grantTurn,
  members,
  type Member,
  FrameRate,
  readFrame,
  refuse,
  revokeTurn,
  send,
  store,
} from "./session";
import {
  DESCRIBE_EVERY_MS,
  FLOOR_MOST,
  PROTOCOL,
  lobbyName,
  ROOM_PEERS,
} from "./wire";

// A public room's row on its lobby's list, kept while the peer that asked for it is in the room.
interface Listing {
  game: string;
  room: string;
  build: string;
  host: string;
  peer: string; // the host's peer id
  floor: number;
  describe_ms: number;
}

interface Peer extends Member {
  build: string;
  seq: number; // orders peers by joining: welcome lists them oldest first, so the first is the host
}

export class Room extends DurableObject<Env> {
  // Lost when the object hibernates, which only forgives a burst.
  private frames = new FrameRate();

  private peers(): { ws: WebSocket; peer: Peer }[] {
    return members<Peer>(this.ctx).map(({ ws, member }) => ({ ws, peer: member }));
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
    const oldest = [...present].sort((a, b) => (a.peer.seq ?? 0) - (b.peer.seq ?? 0)).map(({ peer }) => peer.peer);
    const id = freshPeerId(new Set(oldest));
    const seq = Math.max(0, ...present.map(({ peer }) => peer.seq ?? 0)) + 1;
    this.ctx.acceptWebSocket(server);
    store<Peer>(server, { peer: id, build, ip, turnUsers: [], seq });
    send(server, { type: "welcome", protocol: PROTOCOL, peer: id, peers: oldest });
    for (const { ws: other } of present) send(other, { type: "join", peer: id });
    await this.listOnJoin(new URL(request.url), id, build, present.length === 0);
    return new Response(null, { status: 101, webSocket: client });
  }

  private lobby(listing: Listing) {
    return this.env.LOBBIES.get(this.env.LOBBIES.idFromName(lobbyName(listing.game, listing.build)));
  }

  private async putRow(listing: Listing, players: number): Promise<void> {
    await this.lobby(listing).put({ room: listing.room, host: listing.host, floor: listing.floor, players, most: ROOM_PEERS });
  }

  // The first peer lists the room if it asked to; a later one changes the row's count. A listing
  // left from an earlier session (the object restarted) goes with the room's first new peer.
  private async listOnJoin(url: URL, peer: string, build: string, first: boolean): Promise<void> {
    const listing = await this.ctx.storage.get<Listing>("listing");
    if (!first) {
      if (listing) await this.putRow(listing, this.peers().length);
      return;
    }
    if (listing) {
      await this.ctx.storage.delete("listing");
      await this.lobby(listing).remove(listing.room);
    }
    if (url.searchParams.get("listed") !== "1") return;
    const [, , , game = "", room = ""] = url.pathname.split("/");
    const fresh: Listing = { game, room, build, host: url.searchParams.get("host") ?? "", peer, floor: 0, describe_ms: 0 };
    await this.ctx.storage.put("listing", fresh);
    await this.putRow(fresh, 1);
  }

  private async describe(ws: WebSocket, self: Peer, floor: unknown): Promise<void> {
    const listing = await this.ctx.storage.get<Listing>("listing");
    if (!listing || listing.peer !== self.peer) return; // only the listed room's host
    if (typeof floor !== "number" || !Number.isInteger(floor) || floor < 0 || floor > FLOOR_MOST) {
      fail(ws, "bad_message", `a floor is a whole number 0 to ${FLOOR_MOST}`);
      return;
    }
    const now_ms = Date.now();
    if (now_ms - listing.describe_ms < DESCRIBE_EVERY_MS) return;
    const described = { ...listing, floor, describe_ms: now_ms };
    await this.ctx.storage.put("listing", described);
    await this.putRow(described, this.peers().length);
  }

  async webSocketMessage(ws: WebSocket, message: string | ArrayBuffer): Promise<void> {
    const self = attachment<Peer>(ws);
    if (!self) return; // a refused socket on its way out
    const frame = readFrame(ws, this.frames, message);
    if (!frame) return;
    const type = frame.type;
    if (type === "turn") {
      const others = this.peers().filter(({ ws: other }) => other !== ws).length;
      await grantTurn(this.env, ws, self, others);
      return;
    }
    if (type === "describe") {
      await this.describe(ws, self, frame.floor);
      return;
    }
    if (type !== "signal") {
      fail(ws, "bad_message", "expected a signal, a turn request or a describe");
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
    const self = attachment<Peer>(ws);
    if (!self) return;
    store(ws, null); // a close after an error leaves only once
    const others = this.peers().filter(({ ws: other }) => other !== ws);
    for (const { ws: other } of others) send(other, { type: "leave", peer: self.peer });
    await revokeTurn(this.env, self);
    const listing = await this.ctx.storage.get<Listing>("listing");
    if (!listing) return;
    if (listing.peer !== self.peer) {
      await this.putRow(listing, others.length);
      return;
    }
    // The host is gone, and the session with it.
    await this.ctx.storage.delete("listing");
    await this.lobby(listing).remove(listing.room);
  }
}
