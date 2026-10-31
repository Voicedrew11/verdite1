// The floor mesh against a running service (npm run dev, or SERVICE=https://...): five members join
// one floor, their links are checked from the frames they are sent, signals reach only linked
// peers, and a member left with fewer than two links is given new ones.
import assert from "node:assert/strict";
import { randomBytes } from "node:crypto";

const SERVICE = process.env.SERVICE ?? "http://127.0.0.1:8787";
const WS = SERVICE.replace(/^http/, "ws");
const GAME = `t${randomBytes(4).toString("hex")}`;
const AREA = randomBytes(4).readUInt32LE(0);
const BUILD = `mesh-${randomBytes(3).toString("hex")}`;
const MAX_LINKS = 4;
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// A mesh member: its links are kept from the welcome, join, link and leave frames it is sent.
class Member {
  constructor() {
    this.links = new Set();
    this.events = []; // join and link frames, in order
    this.queue = []; // the other frames, for next()
    this.waiting = [];
    this.ws = new WebSocket(`${WS}/v1/floors/${GAME}/${AREA}/mesh?protocol=1&build=${BUILD}`);
    this.welcomed = new Promise((resolve) => (this.resolveWelcome = resolve));
    this.ws.addEventListener("message", (event) => this.onFrame(JSON.parse(event.data)));
  }

  onFrame(frame) {
    if (frame.type === "welcome") {
      this.peer = frame.peer;
      for (const peer of frame.peers) this.links.add(peer);
      this.resolveWelcome(frame);
      return;
    }
    if (frame.type === "join" || frame.type === "link") {
      this.links.add(frame.peer);
      this.events.push(frame);
      return;
    }
    if (frame.type === "leave") {
      this.links.delete(frame.peer);
      this.events.push(frame);
      return;
    }
    const waiter = this.waiting.shift();
    if (waiter) waiter(frame);
    else this.queue.push(frame);
  }

  next(ms = 5000) {
    if (this.queue.length) return Promise.resolve(this.queue.shift());
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error(`no frame at ${this.peer}`)), ms);
      this.waiting.push((frame) => (clearTimeout(timer), resolve(frame)));
    });
  }

  send(frame) {
    this.ws.send(JSON.stringify(frame));
  }

  close() {
    this.ws.close(1000);
  }
}

async function join() {
  const member = new Member();
  const welcome = await Promise.race([member.welcomed, sleep(10000).then(() => Promise.reject(new Error("no welcome")))]);
  return { member, welcome };
}

const byPeer = (list, peer) => list.find((m) => m.peer === peer);

// Every member has at most MAX_LINKS links, and each link is one both ends agree on.
function checkGraph(list) {
  for (const a of list) {
    assert.ok(a.links.size <= MAX_LINKS, `${a.peer} has ${a.links.size} links`);
    for (const peer of a.links) {
      const b = byPeer(list, peer);
      assert.ok(b, `${a.peer} is linked to ${peer}, which is not in the mesh`);
      assert.ok(b.links.has(a.peer), `${peer} does not have ${a.peer} as a link`);
    }
  }
}

async function check(name, body) {
  await body();
  console.log(`ok   ${name}`);
}

const members = [];
await check("five members join one floor mesh; every welcome has at most 3 peers, all with fewer than 4 links", async () => {
  for (let i = 0; i < 5; i++) {
    const linksBefore = new Map(members.map((m) => [m.peer, m.links.size]));
    const { member, welcome } = await join();
    assert.equal(welcome.type, "welcome");
    assert.equal(welcome.protocol, 1);
    assert.ok(welcome.peers.length <= 3, `welcome has ${welcome.peers.length} peers`);
    for (const peer of welcome.peers) assert.ok(linksBefore.get(peer) < MAX_LINKS, `${peer} had 4 links`);
    members.push(member);
    await sleep(200); // the joins reach the members welcomed
  }
  checkGraph(members);
});

await check("a signal reaches a linked peer, and an unlinked one is unknown_peer", async () => {
  const unlinked = members.flatMap((a) => members.filter((b) => b !== a && !a.links.has(b.peer)).map((b) => [a, b]))[0];
  assert.ok(unlinked, "the mesh is complete, so no unlinked pair to test");
  const [a, b] = unlinked;
  a.send({ type: "signal", to: b.peer, data: { kind: "offer", sdp: "x" } });
  assert.equal((await a.next()).code, "unknown_peer");

  const linked = members.flatMap((c) => [...c.links].map((peer) => [c, byPeer(members, peer)]))[0];
  const [c, d] = linked;
  c.send({ type: "signal", to: d.peer, data: { kind: "offer", sdp: "y" } });
  assert.deepEqual(await d.next(), { type: "signal", from: c.peer, data: { kind: "offer", sdp: "y" } });
});

// Leaves: close a member whose neighbour is left with fewer than 2 links, when one is left with
// a free member to link to; otherwise close the best-connected member and look again.
await check("a member left with fewer than 2 links is given a link or a join", async () => {
  let remaining = [...members];
  for (let step = 0; step < 4; step++) {
    const pick = findDeficit(remaining);
    if (!pick) {
      const busiest = [...remaining].sort((x, y) => y.links.size - x.links.size)[0];
      busiest.close();
      remaining = remaining.filter((m) => m !== busiest);
      await sleep(300);
      checkGraph(remaining);
      continue;
    }
    const { leaver, lacking } = pick;
    const before = new Set(lacking.links);
    const eventsBefore = lacking.events.length;
    leaver.close();
    remaining = remaining.filter((m) => m !== leaver);
    await sleep(400);
    const given = lacking.events.slice(eventsBefore).filter((e) => e.type === "link" || e.type === "join");
    assert.ok(given.length >= 1, `${lacking.peer} had fewer than 2 links and got no link or join`);
    assert.ok(lacking.links.size >= 2, `${lacking.peer} has ${lacking.links.size} links after its relink`);
    assert.ok([...lacking.links].some((peer) => !before.has(peer)), "the new link is new");
    checkGraph(remaining);
    console.log(`     ${lacking.peer} lost a link and got ${given.map((e) => e.type).join(" and ")}`);
    return;
  }
  throw new Error("no leave left a member with fewer than 2 links to relink");
});

// A leave that leaves a member with fewer than 2 links it can still be linked to: a neighbour
// with at most 2 links, and another member, not linked to it, with fewer than 4.
function findDeficit(list) {
  for (const leaver of list) {
    for (const peer of leaver.links) {
      const lacking = byPeer(list, peer);
      if (!lacking || lacking.links.size > 2) continue;
      const free = list.some((m) => m !== leaver && m !== lacking && !lacking.links.has(m.peer) && m.links.size < MAX_LINKS);
      if (free) return { leaver, lacking };
    }
  }
  return null;
}

for (const m of members) m.close();
console.log("all mesh checks passed");
