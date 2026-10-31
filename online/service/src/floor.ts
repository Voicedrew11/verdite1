// A floor (online/protocol/PROTOCOL.md, "Records" and "The floor mesh"): one Durable Object per
// (game, area), named `${game}/${area}`. It serves the floor's records socket and its mesh socket.
// Its records are a cache over D1 (src/store.ts), loaded on first use; its mesh's membership is
// the sockets' attachments. Hibernates between frames.
import { DurableObject } from "cloudflare:workers";
import type { Env } from "./index";
import * as d1 from "./store";
import { sample } from "./sample";
import {
  MESH_LINKS,
  MESH_MIN_LINKS,
  MESH_WELCOME,
  PROTOCOL,
  type SentRecord,
} from "./wire";
import {
  RATING_BYTES,
  RECORD_BYTES,
  decodeRating,
  KEY_BYTES,
  decodeRecord,
  encodeRecord,
  fromBase64,
  idHex,
  recordId,
  toBase64,
  toHex,
  verifyRating,
  verifyRecord,
} from "./record";
import {
  attachment,
  fail,
  freshPeerId,
  grantTurn,
  members,
  readFrame,
  revokeTurn,
  send,
  store,
  FrameRate,
  type Client,
  type Member,
} from "./session";

// A mesh member: its links are the peers it is linked to; seq orders members by joining, so the
// newer of a pair is the one that offers.
interface MeshMember extends Member {
  seq: number;
  links: string[];
}

interface Entry {
  ws: WebSocket;
  member: MeshMember;
}

function sentRecord(held: d1.Held): SentRecord {
  const fields = { area: held.area, place: held.place, payload: held.payload, author: held.author, serial: held.serial, signature: held.signature };
  return { record: toBase64(encodeRecord(fields)), good: held.good };
}

function shuffled<T>(items: T[]): T[] {
  const out = [...items];
  for (let i = out.length - 1; i > 0; i--) {
    const j = Math.floor(Math.random() * (i + 1));
    [out[i], out[j]] = [out[j] as T, out[i] as T];
  }
  return out;
}

function addLink(ws: WebSocket, member: MeshMember, peer: string): void {
  member.links.push(peer);
  store(ws, member);
}

export class Floor extends DurableObject<Env> {
  private game = "";
  private area = 0;
  private frames = new FrameRate();
  private held?: Promise<Map<string, d1.Held>>; // by id as hex; loaded on first use
  private answering = new Map<WebSocket, Promise<void>>(); // each records socket's frame in hand

  constructor(ctx: DurableObjectState, env: Env) {
    super(ctx, env);
    // The name ("game/area") comes with a request by name; kept in storage too, so an object woken by a
    // hibernated socket's message, which may not carry it, still knows its floor.
    void ctx.blockConcurrencyWhile(async () => {
      const name = ctx.id.name ?? (await ctx.storage.get<string>("name")) ?? "";
      if (ctx.id.name) await ctx.storage.put("name", ctx.id.name);
      const [game = "", area = ""] = name.split("/");
      this.game = game;
      this.area = Number(area);
    });
  }

  async fetch(request: Request): Promise<Response> {
    const url = new URL(request.url);
    const ip = request.headers.get("CF-Connecting-IP") ?? "local";
    const [client, server] = Object.values(new WebSocketPair()) as [WebSocket, WebSocket];
    if (url.pathname.endsWith("/mesh")) {
      this.joinMesh(server, ip);
    } else {
      const records = await this.sampleFor(Number(url.searchParams.get("seed")));
      this.ctx.acceptWebSocket(server, ["records"]);
      store<Client>(server, { ip });
      send(server, { type: "records", records });
    }
    return new Response(null, { status: 101, webSocket: client });
  }

  // Drops records retired in this floor's cache: an author's posts in another floor retire these.
  async drop(ids: string[]): Promise<void> {
    const cache = await this.load();
    for (const id of ids) cache.delete(id);
  }

  async webSocketMessage(ws: WebSocket, message: string | ArrayBuffer): Promise<void> {
    if (this.ctx.getTags(ws).includes("mesh")) return this.meshFrame(ws, message);
    // One frame of a records socket at a time, so its answers come in the order it asked: an
    // `error` carries no id, and the client matches it to its oldest request.
    const before = this.answering.get(ws) ?? Promise.resolve();
    const done = before.then(() => this.recordsFrame(ws, message));
    const settled = done.catch(() => {});
    this.answering.set(ws, settled);
    void settled.then(() => {
      if (this.answering.get(ws) === settled) this.answering.delete(ws);
    });
    return done;
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

  private load(): Promise<Map<string, d1.Held>> {
    if (!this.held) {
      this.held = d1.floorRecords(this.env.DB, this.game, this.area).then(
        (rows) => new Map(rows.map((row) => [row.hex, row])),
        (error) => {
          this.held = undefined;
          throw error;
        },
      );
    }
    return this.held;
  }

  private async sampleFor(seed: number): Promise<SentRecord[]> {
    const cache = await this.load();
    return sample([...cache.values()], seed).map(sentRecord);
  }

  private async recordsFrame(ws: WebSocket, message: string | ArrayBuffer): Promise<void> {
    if (!attachment<Client>(ws)) return;
    const frame = readFrame(ws, this.frames, message);
    if (!frame) return;
    switch (frame.type) {
      case "post":
        return this.post(ws, frame.record);
      case "rate":
        return this.rate(ws, frame.rating);
      case "mine":
        return this.mine(ws, frame.author);
      default:
        fail(ws, "bad_message", "expected a post, a rate or a mine");
    }
  }

  private async post(ws: WebSocket, encoded: unknown): Promise<void> {
    const client = attachment<Client>(ws);
    const bytes = fromBase64(encoded, RECORD_BYTES);
    const fields = bytes && decodeRecord(bytes);
    if (!client || !bytes || !fields || fields.area !== this.area || !(await verifyRecord(this.game, fields, bytes))) {
      fail(ws, "refused", "the record is malformed, not this floor's, or not signed by its author");
      return;
    }
    const newest = await d1.newestSerial(this.env.DB, this.game, fields.author);
    if (newest !== null && fields.serial <= newest) {
      fail(ws, "refused", `serial ${fields.serial} is not above the author's newest, ${newest}`);
      return;
    }
    if (!(await this.env.POST_LIMIT.limit({ key: client.ip })).success) {
      fail(ws, "refused", "too many posts from this address");
      return;
    }
    const id = await recordId(bytes);
    const retired = await d1.retiredBy(this.env.DB, this.game, fields.author, fields.serial - 10);
    try {
      await d1.insertPost(this.env.DB, this.game, { ...fields, id });
    } catch (error) {
      console.error(String(error));
      fail(ws, "refused", "the record was not kept");
      return;
    }
    const cache = await this.load();
    await this.forget(retired);
    const hex = idHex(id);
    cache.set(hex, { ...fields, hex, id, good: 0 });
    send(ws, { type: "posted", id: hex, good: 0 });
  }

  // Drops the retired records from the caches of the floors they are on.
  private async forget(retired: { id: Uint8Array; area: number }[]): Promise<void> {
    const byArea = new Map<number, string[]>();
    for (const { id, area } of retired) {
      byArea.set(area, [...(byArea.get(area) ?? []), idHex(id)]);
    }
    for (const [area, ids] of byArea) {
      if (area === this.area) {
        await this.drop(ids);
        continue;
      }
      try {
        await this.floor(area).drop(ids);
      } catch (error) {
        console.error(`floor ${area}: ${String(error)}`);
      }
    }
  }

  private floor(area: number) {
    return this.env.FLOORS.get(this.env.FLOORS.idFromName(`${this.game}/${area}`));
  }

  private async rate(ws: WebSocket, encoded: unknown): Promise<void> {
    const client = attachment<Client>(ws);
    if (!client || !(await this.env.RATE_LIMIT.limit({ key: client.ip })).success) {
      fail(ws, "refused", "too many ratings from this address");
      return;
    }
    const bytes = fromBase64(encoded, RATING_BYTES);
    const rating = bytes && decodeRating(bytes);
    if (!rating || !(await verifyRating(this.game, rating))) {
      fail(ws, "refused", "the rating is malformed or not signed by its rater");
      return;
    }
    const cache = await this.load();
    const record = cache.get(idHex(rating.id));
    if (!record) {
      fail(ws, "refused", "no such record on this floor");
      return;
    }
    if (toHex(record.author) === toHex(rating.rater)) {
      fail(ws, "refused", "a record's author cannot rate it");
      return;
    }
    const { good } = await d1.insertRating(this.env.DB, rating.id, rating.rater);
    record.good = good;
    send(ws, { type: "rated", id: record.hex, good });
  }

  private async mine(ws: WebSocket, encoded: unknown): Promise<void> {
    const client = attachment<Client>(ws);
    if (!client || !(await this.env.MINE_LIMIT.limit({ key: client.ip })).success) {
      fail(ws, "rate_limited", "too many mine requests from this address");
      return;
    }
    const author = fromBase64(encoded, KEY_BYTES);
    if (!author) {
      fail(ws, "bad_message", "author is a base64 key of 32 bytes");
      return;
    }
    const rows = await d1.mineRecords(this.env.DB, this.game, author);
    send(ws, { type: "mine", records: rows.map(sentRecord) });
  }

  // The mesh: welcome gives the newcomer up to MESH_WELCOME members with fewer than MESH_LINKS
  // links; only those are told of it.
  private joinMesh(server: WebSocket, ip: string): void {
    const present = members<MeshMember>(this.ctx, "mesh");
    const id = freshPeerId(new Set(present.map(({ member }) => member.peer)));
    const seq = Math.max(0, ...present.map(({ member }) => member.seq)) + 1;
    const chosen = shuffled(present.filter(({ member }) => member.links.length < MESH_LINKS)).slice(0, MESH_WELCOME);
    this.ctx.acceptWebSocket(server, ["mesh"]);
    const me: MeshMember = { peer: id, ip, turnUsers: [], seq, links: chosen.map(({ member }) => member.peer) };
    store(server, me);
    send(server, { type: "welcome", protocol: PROTOCOL, peer: id, peers: me.links });
    for (const { ws, member } of chosen) {
      addLink(ws, member, id);
      send(ws, { type: "join", peer: id });
    }
  }

  private async meshFrame(ws: WebSocket, message: string | ArrayBuffer): Promise<void> {
    const self = attachment<MeshMember>(ws);
    if (!self) return;
    const frame = readFrame(ws, this.frames, message);
    if (!frame) return;
    if (frame.type === "turn") {
      await grantTurn(this.env, ws, self, self.links.length);
      return;
    }
    if (frame.type === "describe") return; // ignored on a mesh
    if (frame.type !== "signal") {
      fail(ws, "bad_message", "expected a signal, a turn request or a describe");
      return;
    }
    const { to, data } = frame as { to?: unknown; data?: unknown };
    const target =
      typeof to === "string" && self.links.includes(to)
        ? members<MeshMember>(this.ctx, "mesh").find(({ member }) => member.peer === to)
        : undefined;
    if (!target) {
      fail(ws, "unknown_peer", `no linked peer ${String(to)}`);
      return;
    }
    send(target.ws, { type: "signal", from: self.peer, data });
  }

  // A mesh member closing: its links hear it, its TURN credentials go, and a link left with fewer
  // than MESH_MIN_LINKS is given new ones.
  private async leave(ws: WebSocket): Promise<void> {
    if (!this.ctx.getTags(ws).includes("mesh")) return;
    const self = attachment<MeshMember>(ws);
    if (!self) return;
    store(ws, null); // a close after an error leaves only once
    const others = members<MeshMember>(this.ctx, "mesh").filter(({ ws: other }) => other !== ws);
    const linked = others.filter(({ member }) => self.links.includes(member.peer));
    for (const other of linked) {
      other.member.links = other.member.links.filter((peer) => peer !== self.peer);
      store(other.ws, other.member);
      send(other.ws, { type: "leave", peer: self.peer });
    }
    await revokeTurn(this.env, self);
    for (const other of linked) {
      if (other.member.links.length < MESH_MIN_LINKS) this.relink(other, others);
    }
  }

  private relink(entry: Entry, all: Entry[]): void {
    while (entry.member.links.length < MESH_MIN_LINKS) {
      const options = all.filter(
        ({ ws, member }) =>
          ws !== entry.ws && member.links.length < MESH_LINKS && !entry.member.links.includes(member.peer),
      );
      const pick = shuffled(options)[0];
      if (!pick) return;
      const [newer, older] = entry.member.seq > pick.member.seq ? [entry, pick] : [pick, entry];
      addLink(entry.ws, entry.member, pick.member.peer);
      addLink(pick.ws, pick.member, entry.member.peer);
      send(newer.ws, { type: "link", peer: older.member.peer });
      send(older.ws, { type: "join", peer: newer.member.peer });
    }
  }
}
