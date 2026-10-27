// The list of public rooms for one (game, build): rooms put and remove their rows, and every open
// socket is sent the list when it opens and whenever it changes, at most once a second
// (online/protocol/PROTOCOL.md, "The list of public rooms"). Hibernates between changes; a row
// its room has not refreshed for ROW_STALE_MS (a room gone without saying so) is dropped.
import { DurableObject } from "cloudflare:workers";
import type { Env } from "./index";
import { LISTED_ROOMS, type ListedRoom, type ServerFrame } from "./wire";

const SEND_EVERY_MS = 1000;
export const ROW_STALE_MS = 5 * 60 * 1000;
const PRUNE_EVERY_MS = 60 * 1000;

interface Row extends ListedRoom {
  since_ms: number; // listed at
  seen_ms: number; // last put
}

function send(ws: WebSocket, frame: ServerFrame): void {
  try {
    ws.send(JSON.stringify(frame));
  } catch {
    // closing
  }
}

export class Lobby extends DurableObject<Env> {
  async fetch(_request: Request): Promise<Response> {
    const [client, server] = Object.values(new WebSocketPair()) as [WebSocket, WebSocket];
    this.ctx.acceptWebSocket(server);
    send(server, await this.frame());
    return new Response(null, { status: 101, webSocket: client });
  }

  // From a room: its row, new or changed.
  async put(room: ListedRoom): Promise<void> {
    const now_ms = Date.now();
    const old = await this.ctx.storage.get<Row>(`row:${room.room}`);
    await this.ctx.storage.put<Row>(`row:${room.room}`, { ...room, since_ms: old?.since_ms ?? now_ms, seen_ms: now_ms });
    const same = old && old.host === room.host && old.floor === room.floor && old.players === room.players && old.most === room.most;
    if (!same) await this.changed();
  }

  async remove(room: string): Promise<void> {
    if (await this.ctx.storage.delete(`row:${room}`)) await this.changed();
  }

  // Clients send nothing; a frame from one is ignored.
  async webSocketMessage(): Promise<void> {}

  async webSocketClose(ws: WebSocket, code: number): Promise<void> {
    try {
      ws.close(code === 1005 ? 1000 : code, "bye");
    } catch {
      // already closed
    }
  }

  async alarm(): Promise<void> {
    const now_ms = Date.now();
    const rows = await this.ctx.storage.list<Row>({ prefix: "row:" });
    const stale = [...rows].filter(([, row]) => now_ms - row.seen_ms > ROW_STALE_MS).map(([key]) => key);
    if (stale.length) {
      await this.ctx.storage.delete(stale);
      await this.ctx.storage.put("pending", true);
    }
    if (await this.ctx.storage.get<boolean>("pending")) await this.broadcast();
    if (rows.size > stale.length) await this.ctx.storage.setAlarm(now_ms + PRUNE_EVERY_MS);
  }

  private async changed(): Promise<void> {
    const now_ms = Date.now();
    const last_ms = (await this.ctx.storage.get<number>("sent_ms")) ?? 0;
    if (now_ms - last_ms >= SEND_EVERY_MS) {
      await this.broadcast();
      const alarm_ms = await this.ctx.storage.getAlarm();
      if (alarm_ms === null) await this.ctx.storage.setAlarm(now_ms + PRUNE_EVERY_MS);
      return;
    }
    await this.ctx.storage.put("pending", true);
    const due_ms = last_ms + SEND_EVERY_MS;
    const alarm_ms = await this.ctx.storage.getAlarm();
    if (alarm_ms === null || alarm_ms > due_ms) await this.ctx.storage.setAlarm(due_ms);
  }

  private async broadcast(): Promise<void> {
    await this.ctx.storage.put({ sent_ms: Date.now(), pending: false });
    const frame = await this.frame();
    for (const ws of this.ctx.getWebSockets()) send(ws, frame);
  }

  private async frame(): Promise<ServerFrame> {
    const rows = [...(await this.ctx.storage.list<Row>({ prefix: "row:" })).values()];
    rows.sort((a, b) => b.since_ms - a.since_ms);
    return {
      type: "rooms",
      rooms: rows.slice(0, LISTED_ROOMS).map(({ room, host, floor, players, most }) => ({ room, host, floor, players, most })),
    };
  }
}
