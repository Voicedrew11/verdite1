// A deployed service hands out TURN servers (SERVICE=https://...). Prints the URLs and whether a
// username and credential came, never their values.
import assert from "node:assert/strict";

const SERVICE = process.env.SERVICE;
if (!SERVICE) throw new Error("set SERVICE to the deployed Worker's URL");
const room = `turn-check-${Math.random().toString(36).slice(2, 10)}`;
const ws = new WebSocket(`${SERVICE.replace(/^http/, "ws")}/v1/rooms/test/${room}?protocol=1&build=check`);

const welcome = await new Promise((resolve, reject) => {
  const timer = setTimeout(() => reject(new Error("no welcome")), 10000);
  ws.addEventListener("message", (event) => (clearTimeout(timer), resolve(JSON.parse(event.data))));
  ws.addEventListener("error", () => reject(new Error("could not connect")));
});
ws.close(1000);

assert.equal(welcome.type, "welcome", `got ${welcome.type} ${welcome.code ?? ""}`);
const turn = welcome.iceServers.filter((server) =>
  [server.urls].flat().some((url) => /^turns?:/.test(url)),
);
for (const server of welcome.iceServers)
  console.log([server.urls].flat().join(" "), server.username ? "(username, credential)" : "");
assert.ok(turn.length > 0, "no TURN servers: the Worker has no TURN key, or it is wrong");
assert.ok(turn.every((server) => server.username && server.credential), "TURN server without a credential");
assert.ok(
  welcome.iceServers.every((server) => [server.urls].flat().every((url) => !/:53(\?|$)/.test(url))),
  "port 53 was not filtered",
);
console.log("ok   TURN servers with credentials, port 53 filtered");
