// The Verdite online service: routes a room's WebSocket to its Durable Object
// (online/protocol/PROTOCOL.md) and checks the month's TURN budget on a schedule.
import { Budget, checkBudget } from "./budget";
import { Floor } from "./floor";
import { Lobby } from "./lobby";
import { Room } from "./room";
import { FLOOR_AREA_MOST, HOST_NAME, PROTOCOL, PUBLIC_ROOM, lobbyName, refusal } from "./wire";

export { Budget, Floor, Lobby, Room };

export interface Env {
  ROOMS: DurableObjectNamespace<Room>;
  LOBBIES: DurableObjectNamespace<Lobby>; // one per (game, build)
  BUDGET: DurableObjectNamespace<Budget>;
  FLOORS: DurableObjectNamespace<Floor>; // one per (game, area): records and the floor mesh
  DB: D1Database; // the records (migrations/)
  JOIN_LIMIT: RateLimit; // joins per address
  TURN_LIMIT: RateLimit; // TURN credentials per address
  POST_LIMIT: RateLimit; // record posts per address
  RATE_LIMIT: RateLimit; // ratings per address: each can come from a fresh key
  MINE_LIMIT: RateLimit; // mine requests per address
  TURN_BUDGET_GB?: string;
  TURN_CREDENTIAL_SECONDS?: string; // 3600 when unset; shorter only to test renewal
  // secrets (online/README.md, "Secrets")
  TURN_KEY_ID?: string;
  TURN_KEY_API_TOKEN?: string;
  CF_ACCOUNT_ID?: string;
  CF_ANALYTICS_TOKEN?: string;
}

const GAME = /^[a-z0-9]{1,16}$/;
const ROOM = /^[A-Za-z0-9_-]{1,64}$/;
const BUILD = /^[A-Za-z0-9._+-]{1,64}$/;
const WHOLE = /^(0|[1-9][0-9]{0,9})$/; // a decimal number up to 4294967295, checked below

// The checks a join and the list share; null when the request may go on. A build is checked when
// given (records do not carry one).
async function refusedUpgrade(request: Request, env: Env, game: string, build?: string): Promise<Response | null> {
  const url = new URL(request.url);
  if (!GAME.test(game) || (build !== undefined && !BUILD.test(build)))
    return refusal(400, "bad_request", "game or build is malformed");
  if (url.searchParams.get("protocol") !== String(PROTOCOL))
    return refusal(400, "protocol", `this service speaks protocol ${PROTOCOL}`);
  if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket")
    return refusal(400, "bad_request", "expected a WebSocket upgrade");
  const ip = request.headers.get("CF-Connecting-IP") ?? "local";
  if (!(await env.JOIN_LIMIT.limit({ key: ip })).success)
    return refusal(429, "rate_limited", "too many joins from this address");
  return null;
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);
    const build = url.searchParams.get("build") ?? "";
    const lobby = url.pathname.match(/^\/v1\/lobby\/([^/]+)$/);
    if (lobby) {
      const [, game = ""] = lobby;
      const refused = await refusedUpgrade(request, env, game, build);
      if (refused) return refused;
      return env.LOBBIES.get(env.LOBBIES.idFromName(lobbyName(game, build))).fetch(request);
    }
    const floor = url.pathname.match(/^\/v1\/floors\/([^/]+)\/([^/]+)\/(records|mesh)$/);
    if (floor) {
      const [, game = "", area = "", kind = ""] = floor;
      if (!WHOLE.test(area) || Number(area) > FLOOR_AREA_MOST) return refusal(400, "bad_request", "the area is malformed");
      const seed = url.searchParams.get("seed") ?? "";
      if (kind === "records" && (!WHOLE.test(seed) || Number(seed) > FLOOR_AREA_MOST))
        return refusal(400, "bad_request", "the seed is a whole number 0 to 4294967295");
      const refused = await refusedUpgrade(request, env, game, kind === "mesh" ? build : undefined);
      if (refused) return refused;
      return env.FLOORS.get(env.FLOORS.idFromName(`${game}/${area}`)).fetch(request);
    }
    const path = url.pathname.match(/^\/v1\/rooms\/([^/]+)\/([^/]+)$/);
    if (!path) return refusal(404, "not_found", "no such path");
    const [, game = "", room = ""] = path;
    if (!ROOM.test(room)) return refusal(400, "bad_request", "the room is malformed");
    if (url.searchParams.get("listed") === "1" && (!PUBLIC_ROOM.test(room) || !HOST_NAME.test(url.searchParams.get("host") ?? "")))
      return refusal(400, "not_listable", "only a public room is listed, with a host name");
    const refused = await refusedUpgrade(request, env, game, build);
    if (refused) return refused;

    const stub = env.ROOMS.get(env.ROOMS.idFromName(`${game}/${room}`));
    return stub.fetch(request);
  },

  async scheduled(_controller: ScheduledController, env: Env, ctx: ExecutionContext): Promise<void> {
    ctx.waitUntil(checkBudget(env));
  },
} satisfies ExportedHandler<Env>;
