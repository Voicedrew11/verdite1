// The signaling protocol against a running service (npm run dev, or SERVICE=https://...).
// Each case uses a fresh room, so it can run against a deployed service too.
import assert from "node:assert/strict";
import { request as httpRequest } from "node:http";
import { request as httpsRequest } from "node:https";
import { randomBytes } from "node:crypto";

const SERVICE = process.env.SERVICE ?? "http://127.0.0.1:8787";
const WS = SERVICE.replace(/^http/, "ws");
const run = Math.random().toString(36).slice(2, 10);

function roomUrl(room, build = "test-1", protocol = "1") {
  return `${WS}/v1/rooms/test/${run}-${room}?protocol=${protocol}&build=${build}`;
}

// A socket whose frames are read in order with next().
function connect(url) {
  const ws = new WebSocket(url);
  const frames = [];
  const waiting = [];
  ws.addEventListener("message", (event) => {
    const frame = JSON.parse(event.data);
    if (process.env.DEBUG) console.log("  <-", frame.type, frame.code ?? frame.peer ?? frame.from ?? "");
    const waiter = waiting.shift();
    if (waiter) waiter(frame);
    else frames.push(frame);
  });
  const closed = new Promise((resolve) => ws.addEventListener("close", (event) => resolve(event)));
  return {
    ws,
    closed,
    send: (frame) => ws.send(typeof frame === "string" ? frame : JSON.stringify(frame)),
    // a welcome waits on a TURN credential from Cloudflare, so give it time
    next: (ms = 10000) =>
      frames.length
        ? Promise.resolve(frames.shift())
        : new Promise((resolve, reject) => {
            const timer = setTimeout(() => reject(new Error("no frame")), ms);
            waiting.push((frame) => (clearTimeout(timer), resolve(frame)));
          }),
    close: () => ws.close(1000),
  };
}

const cases = {
  async "two peers are introduced once, and signals are relayed"() {
    const a = connect(roomUrl("pair"));
    const wa = await a.next();
    assert.equal(wa.type, "welcome");
    assert.equal(wa.protocol, 1);
    assert.match(wa.peer, /^[0-9a-f]{16}$/);
    assert.deepEqual(wa.peers, []);
    assert.equal(wa.iceServers, undefined); // credentials only on request

    const b = connect(roomUrl("pair"));
    const wb = await b.next();
    assert.deepEqual(wb.peers, [wa.peer]);
    assert.deepEqual(await a.next(), { type: "join", peer: wb.peer });

    b.send({ type: "signal", to: wa.peer, data: { kind: "offer", sdp: "v=0" } });
    assert.deepEqual(await a.next(), { type: "signal", from: wb.peer, data: { kind: "offer", sdp: "v=0" } });
    a.send({ type: "signal", to: wb.peer, data: { kind: "answer", sdp: "v=0" } });
    assert.deepEqual(await b.next(), { type: "signal", from: wa.peer, data: { kind: "answer", sdp: "v=0" } });

    b.close();
    assert.deepEqual(await a.next(), { type: "leave", peer: wb.peer });
    a.close();
  },

  async "a TURN credential only for a peer with someone to connect to"() {
    const a = connect(roomUrl("turn"));
    await a.next();
    a.send({ type: "turn" });
    assert.equal((await a.next()).code, "alone");

    const b = connect(roomUrl("turn"));
    await b.next();
    assert.equal((await a.next()).type, "join");
    b.send({ type: "turn" });
    const turn = await b.next();
    assert.equal(turn.type, "turn");
    assert.ok(Array.isArray(turn.iceServers)); // empty on a service without a TURN key
    assert.equal(turn.seconds, 3600);
    a.close();
    b.close();
  },

  async "a fifth peer is refused"() {
    const peers = [];
    for (let i = 0; i < 4; i++) {
      const p = connect(roomUrl("full"));
      assert.equal((await p.next()).type, "welcome");
      peers.push(p);
    }
    const fifth = connect(roomUrl("full"));
    assert.equal((await fifth.next()).code, "full");
    await fifth.closed;
    peers.forEach((p) => p.close());
  },

  async "another build is refused"() {
    const a = connect(roomUrl("build", "test-1"));
    assert.equal((await a.next()).type, "welcome");
    const b = connect(roomUrl("build", "test-2"));
    assert.equal((await b.next()).code, "build_mismatch");
    await b.closed;
    a.close();
  },

  async "bad frames and unknown peers are errors, not disconnects"() {
    const a = connect(roomUrl("bad"));
    await a.next();
    a.send("not json");
    assert.equal((await a.next()).code, "bad_message");
    a.send({ type: "hello" });
    assert.equal((await a.next()).code, "bad_message");
    a.send({ type: "signal", to: "0000000000000000", data: {} });
    assert.equal((await a.next()).code, "unknown_peer");
    a.ws.send(new Uint8Array([1, 2, 3]));
    assert.equal((await a.next()).code, "bad_message");
    a.send({ type: "signal", to: "x", data: "y".repeat(70 * 1024) });
    assert.equal((await a.next()).code, "bad_message");
    assert.equal(a.ws.readyState, WebSocket.OPEN);
    a.close();
  },

  async "the request is checked before the upgrade"() {
    // fetch() will not send Upgrade, so a plain HTTP request asks for one and reads the refusal
    const refused = (path, upgrade = true) =>
      new Promise((resolve, reject) => {
        // a complete handshake: Cloudflare's edge turns away an incomplete one before the Worker
        const headers = upgrade
          ? { Connection: "Upgrade", Upgrade: "websocket", "Sec-WebSocket-Version": "13",
              "Sec-WebSocket-Key": randomBytes(16).toString("base64") }
          : {};
        const request = SERVICE.startsWith("https:") ? httpsRequest : httpRequest;
        const req = request(`${SERVICE}${path}`, { headers }, (res) => {
          let body = "";
          res.on("data", (chunk) => (body += chunk));
          res.on("end", () => resolve({ status: res.statusCode, error: JSON.parse(body).error }));
        });
        req.on("upgrade", () => reject(new Error(`${path} was upgraded`)));
        req.on("error", reject);
        req.end();
      });
    assert.deepEqual(await refused("/v1/rooms/test/r?protocol=2&build=b"), { status: 400, error: "protocol" });
    assert.deepEqual(await refused("/v1/rooms/Bad%20Game/r?protocol=1&build=b"), { status: 400, error: "bad_request" });
    assert.deepEqual(await refused("/v1/rooms/test/r?protocol=1"), { status: 400, error: "bad_request" });
    assert.deepEqual(await refused("/v1/rooms/test/r?protocol=1&build=b", false), { status: 400, error: "bad_request" });
    assert.deepEqual(await refused("/nowhere"), { status: 404, error: "not_found" });
  },
};

let failed = 0;
for (const [name, test] of Object.entries(cases)) {
  try {
    await test();
    console.log(`ok   ${name}`);
  } catch (error) {
    failed += 1;
    console.log(`FAIL ${name}\n     ${error.stack}`);
  }
}
process.exit(failed ? 1 : 0);
