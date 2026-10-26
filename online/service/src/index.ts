// The Verdite online service: routes a room's WebSocket to its Durable Object
// (online/protocol/PROTOCOL.md) and checks the month's TURN budget on a schedule.
import { Budget, checkBudget } from "./budget";
import { Room } from "./room";
import { PROTOCOL, refusal } from "./wire";

export { Budget, Room };

export interface Env {
  ROOMS: DurableObjectNamespace<Room>;
  BUDGET: DurableObjectNamespace<Budget>;
  JOIN_LIMIT: RateLimit; // joins per address
  TURN_LIMIT: RateLimit; // TURN credentials per address
  TURN_BUDGET_GB?: string;
  // secrets (online/README.md, "Secrets")
  TURN_KEY_ID?: string;
  TURN_KEY_API_TOKEN?: string;
  CF_ACCOUNT_ID?: string;
  CF_ANALYTICS_TOKEN?: string;
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
    const ip = request.headers.get("CF-Connecting-IP") ?? "local";
    if (!(await env.JOIN_LIMIT.limit({ key: ip })).success)
      return refusal(429, "rate_limited", "too many joins from this address");

    const stub = env.ROOMS.get(env.ROOMS.idFromName(`${game}/${room}`));
    return stub.fetch(request);
  },

  async scheduled(_controller: ScheduledController, env: Env, ctx: ExecutionContext): Promise<void> {
    ctx.waitUntil(checkBudget(env));
  },
} satisfies ExportedHandler<Env>;
