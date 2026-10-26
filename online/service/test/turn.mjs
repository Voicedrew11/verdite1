// A deployed service hands a peer with someone to connect to TURN servers (SERVICE=https://...).
// Prints the URLs and whether a username and credential came, never their values.
import assert from "node:assert/strict";

const SERVICE = process.env.SERVICE;
if (!SERVICE) throw new Error("set SERVICE to the deployed Worker's URL");
const room = `turn-check-${Math.random().toString(36).slice(2, 10)}`;
const url = `${SERVICE.replace(/^http/, "ws")}/v1/rooms/test/${room}?protocol=1&build=check`;

// The first frame of a type; a credential needs a second peer in the room.
function open() {
  const ws = new WebSocket(url);
  const frames = [];
  ws.addEventListener("message", (event) => frames.push(JSON.parse(event.data)));
  const wait = (type) =>
    new Promise((resolve, reject) => {
      const start = Date.now();
      const poll = setInterval(() => {
        const frame = frames.find((f) => f.type === type || f.type === "error");
        if (frame) (clearInterval(poll), resolve(frame));
        else if (Date.now() - start > 10000) (clearInterval(poll), reject(new Error(`no ${type}`)));
      }, 20);
    });
  return { ws, wait };
}
const a = open();
await a.wait("welcome");
const b = open();
await b.wait("welcome");
b.ws.send(JSON.stringify({ type: "turn" }));
const reply = await b.wait("turn");
a.ws.close(1000);
b.ws.close(1000);

assert.equal(reply.type, "turn", `got ${reply.type} ${reply.code ?? ""}`);
assert.equal(reply.seconds, 3600);
const turn = reply.iceServers.filter((server) =>
  [server.urls].flat().some((url) => /^turns?:/.test(url)),
);
for (const server of reply.iceServers)
  console.log([server.urls].flat().join(" "), server.username ? "(username, credential)" : "");
assert.ok(turn.length > 0, "no TURN servers: the Worker has no TURN key, or it is wrong");
assert.ok(turn.every((server) => server.username && server.credential), "TURN server without a credential");
assert.ok(
  reply.iceServers.every((server) => [server.urls].flat().every((url) => !/:53(\?|$)/.test(url))),
  "port 53 was not filtered",
);
console.log("ok   TURN servers with credentials, port 53 filtered");
