// Short-lived TURN credentials from Cloudflare Realtime, per peer, revoked when it leaves. The
// key is the service's secret; without it there are no ICE servers (local testing).

const API = "https://rtc.live.cloudflare.com/v1/turn/keys";
export const CREDENTIAL_SECONDS = 60 * 60; // a longer session asks again

export interface TurnKey {
  TURN_KEY_ID?: string;
  TURN_KEY_API_TOKEN?: string;
}

export interface Credential {
  iceServers: RTCIceServerJson[];
  username: string | null; // to revoke; null without a key
}

interface RTCIceServerJson {
  urls: string | string[];
  username?: string;
  credential?: string;
}

function configured(key: TurnKey): key is Required<TurnKey> {
  return !!key.TURN_KEY_ID && !!key.TURN_KEY_API_TOKEN;
}

// Port 53 is blocked by browsers and many networks; without trickle ICE it only adds a timeout.
function withoutPort53(server: RTCIceServerJson): RTCIceServerJson {
  const urls = (Array.isArray(server.urls) ? server.urls : [server.urls]).filter(
    (url) => !/:53(\?|$)/.test(url),
  );
  return { ...server, urls };
}

export async function issue(key: TurnKey): Promise<Credential> {
  if (!configured(key)) return { iceServers: [], username: null };
  const response = await fetch(`${API}/${key.TURN_KEY_ID}/credentials/generate-ice-servers`, {
    method: "POST",
    headers: { Authorization: `Bearer ${key.TURN_KEY_API_TOKEN}`, "Content-Type": "application/json" },
    body: JSON.stringify({ ttl: CREDENTIAL_SECONDS }),
  });
  if (!response.ok) throw new Error(`TURN credentials: HTTP ${response.status}`);
  const body = (await response.json()) as { iceServers?: RTCIceServerJson[] };
  const iceServers = (body.iceServers ?? []).map(withoutPort53);
  const username = iceServers.find((server) => server.username)?.username ?? null;
  return { iceServers, username };
}

export async function revoke(key: TurnKey, username: string): Promise<void> {
  if (!configured(key)) return;
  const response = await fetch(
    `${API}/${key.TURN_KEY_ID}/credentials/${encodeURIComponent(username)}/revoke`,
    { method: "POST", headers: { Authorization: `Bearer ${key.TURN_KEY_API_TOKEN}` } },
  );
  if (!response.ok && response.status !== 404)
    console.error(`TURN revoke: HTTP ${response.status}`);
}
