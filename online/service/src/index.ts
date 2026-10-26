// The Verdite online service: routes a room's WebSocket to its Durable Object
// (online/protocol/PROTOCOL.md).
import { Room } from "./room";
import { PROTOCOL, refusal } from "./wire";

export { Room };

export interface Env {
  ROOMS: DurableObjectNamespace<Room>;
  TURN_KEY_ID?: string;
  TURN_KEY_API_TOKEN?: string;
}

const GAME = /^[a-z0-9]{1,16}$/;
const ROOM = /^[A-Za-z0-9_-]{1,64}$/;
const BUILD = /^[A-Za-z0-9._+-]{1,64}$/;

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);
    const path = url.pathname.match(/^\/v1\/rooms\/([^/]+)\/([^/]+)$/);
    if (!path) return refusal(404, "not_found", "no such path");
    const [, game = "", room = ""] = path;
    const build = url.searchParams.get("build") ?? "";
    if (!GAME.test(game) || !ROOM.test(room) || !BUILD.test(build))
      return refusal(400, "bad_request", "game, room or build is malformed");
    if (url.searchParams.get("protocol") !== String(PROTOCOL))
      return refusal(400, "protocol", `this service speaks protocol ${PROTOCOL}`);
    if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket")
      return refusal(400, "bad_request", "expected a WebSocket upgrade");

    const stub = env.ROOMS.get(env.ROOMS.idFromName(`${game}/${room}`));
    return stub.fetch(request);
  },
} satisfies ExportedHandler<Env>;
