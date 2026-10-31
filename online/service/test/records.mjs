// The records protocol against a running service (npm run dev, or SERVICE=https://...):
// posts and their refusals, ratings, samples, mine and retirement, on fresh floors of a fresh game.
// Posts are limited to 10 a minute per address, so the test waits out one window (about a minute).
import assert from "node:assert/strict";
import { createHash, generateKeyPairSync, randomBytes, sign } from "node:crypto";

const SERVICE = process.env.SERVICE ?? "http://127.0.0.1:8787";
const WS = SERVICE.replace(/^http/, "ws");
const GAME = `t${randomBytes(4).toString("hex")}`;
const POST_WINDOW_MS = 61_000;

// An install: its Ed25519 key, as the 32 raw bytes a record carries.
function author() {
  const { publicKey, privateKey } = generateKeyPairSync("ed25519");
  const der = publicKey.export({ format: "der", type: "spki" });
  return { key: der.subarray(der.length - 32), privateKey };
}

function floorArea() {
  return randomBytes(4).readUInt32LE(0);
}

// A record per PROTOCOL.md: the 54-byte body, then the signature over the domain, a zero byte and
// the body. signer and area can be wrong on purpose.
function recordBytes({ by, area, place = 0, payload = Buffer.alloc(0), serial, signer = by.privateKey }) {
  const body = Buffer.alloc(54);
  body[0] = 1;
  body.writeUInt32LE(area, 1);
  body.writeUInt32LE(place, 5);
  body[9] = payload.length;
  payload.copy(body, 10);
  by.key.copy(body, 18);
  body.writeUInt32LE(serial, 50);
  const signed = Buffer.concat([Buffer.from(`verdite-record/${GAME}`), Buffer.from([0]), body]);
  return Buffer.concat([body, sign(null, signed, signer)]);
}

// The id: the first 8 bytes of the SHA-256 of the body, little-endian, 0 made 1; as 16 hex digits.
function idOf(bytes) {
  const digest = createHash("sha256").update(bytes.subarray(0, 54)).digest();
  let value = digest.readBigUInt64LE(0);
  if (value === 0n) value = 1n;
  return value.toString(16).padStart(16, "0");
}

// A rating: the record's id (8 bytes), the rater's key (32), a signature over the domain and those.
function ratingBytes(rater, id) {
  const body = Buffer.alloc(40);
  body.writeBigUInt64LE(BigInt(`0x${id}`), 0);
  rater.key.copy(body, 8);
  const signed = Buffer.concat([Buffer.from(`verdite-rating/${GAME}`), Buffer.from([0]), body]);
  return Buffer.concat([body, sign(null, signed, rater.privateKey)]);
}

const serialOf = (bytes) => bytes.readUInt32LE(50);
const cell = (k) => ((k - 1) % 8) | (Math.floor((k - 1) / 8) << 8); // cells 0 to 7 of block (0, 0) for k up to 16

// A socket whose frames are read in order with next().
function connect(url) {
  const ws = new WebSocket(url);
  const frames = [];
  const waiting = [];
  ws.addEventListener("message", (event) => {
    const frame = JSON.parse(event.data);
    if (process.env.DEBUG) console.log("  <-", frame.type, frame.code ?? "");
    const waiter = waiting.shift();
    if (waiter) waiter(frame);
    else frames.push(frame);
  });
  return {
    send: (frame) => ws.send(JSON.stringify(frame)),
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

const floorUrl = (area, seed) => `${WS}/v1/floors/${GAME}/${area}/records?protocol=1&seed=${seed}`;
const b64 = (bytes) => bytes.toString("base64");
const sampleIds = (frame) => frame.records.map((entry) => idOf(Buffer.from(entry.record, "base64")));

async function check(name, body) {
  await body();
  console.log(`ok   ${name}`);
}

const X = author();
const R = author(); // a rater
const Y = author(); // a stranger's key, for a bad signature
const F1 = floorArea();
const F2 = floorArea();
const records = new Map(); // serial of X's record on F1 to its bytes

const s = connect(floorUrl(F1, 7));
await check("a new floor's sample is empty", async () => {
  assert.deepEqual(await s.next(), { type: "records", records: [] });
});

const rec1 = recordBytes({ by: X, area: F1, place: 0, serial: 1 });
records.set(1, rec1);
await check("a post is accepted with its id, and a fresh sample has it", async () => {
  s.send({ type: "post", record: b64(rec1) });
  assert.deepEqual(await s.next(), { type: "posted", id: idOf(rec1), good: 0 });
  const fresh = connect(floorUrl(F1, 3));
  const sample = await fresh.next();
  assert.equal(sample.type, "records");
  assert.deepEqual(sampleIds(sample), [idOf(rec1)]);
  assert.equal(sample.records[0].good, 0);
  fresh.close();
});

await check("a bad signature, another floor, malformed bytes and a serial not above the newest are refused", async () => {
  const refusals = [
    ["a bad signature", recordBytes({ by: X, area: F1, serial: 2, signer: Y.privateKey })],
    ["another floor's area", recordBytes({ by: X, area: F2, serial: 2 })],
    ["117 bytes", recordBytes({ by: X, area: F1, serial: 2 }).subarray(0, 117)],
    ["serial 1 again", recordBytes({ by: X, area: F1, serial: 1, place: 5 })],
  ];
  for (const [label, bytes] of refusals) {
    s.send({ type: "post", record: b64(bytes) });
    const frame = await s.next();
    assert.equal(frame.type, "error", label);
    assert.equal(frame.code, "refused", label);
  }
});

await check("a rating counts once, is refused for one's own record, a bad signature or an unknown record", async () => {
  s.send({ type: "rate", rating: b64(ratingBytes(R, idOf(rec1))) });
  assert.deepEqual(await s.next(), { type: "rated", id: idOf(rec1), good: 1 });
  s.send({ type: "rate", rating: b64(ratingBytes(R, idOf(rec1))) });
  assert.deepEqual(await s.next(), { type: "rated", id: idOf(rec1), good: 1 });
  s.send({ type: "rate", rating: b64(ratingBytes(X, idOf(rec1))) });
  assert.equal((await s.next()).code, "refused");
  const tampered = ratingBytes(R, idOf(rec1));
  tampered[60] ^= 1;
  s.send({ type: "rate", rating: b64(tampered) });
  assert.equal((await s.next()).code, "refused");
  s.send({ type: "rate", rating: b64(ratingBytes(R, "0000000000000002")) });
  assert.equal((await s.next()).code, "refused");
});

await check("serials 2 to 10 in one block are posted, and an eleventh within the minute is refused", async () => {
  for (let k = 2; k <= 10; k++) {
    const bytes = recordBytes({ by: X, area: F1, place: cell(k), serial: k });
    records.set(k, bytes);
    s.send({ type: "post", record: b64(bytes) });
    assert.equal((await s.next()).type, "posted", `serial ${k}`);
  }
  const eleventh = recordBytes({ by: X, area: F1, place: cell(11), serial: 11 });
  s.send({ type: "post", record: b64(eleventh) });
  const frame = await s.next();
  assert.equal(frame.code, "refused", "the rate limit");
});

s.close();
console.log(`     waiting ${POST_WINDOW_MS / 1000} s for the post window to pass`);
await new Promise((resolve) => setTimeout(resolve, POST_WINDOW_MS));

const t = connect(floorUrl(F2, 9));
await check("serials 11 and 12 on a second floor retire serials 1 and 2", async () => {
  assert.deepEqual(await t.next(), { type: "records", records: [] });
  const rec11 = recordBytes({ by: X, area: F2, place: 0, serial: 11 });
  const rec12 = recordBytes({ by: X, area: F2, place: cell(12), serial: 12, payload: Buffer.from([1, 2, 3, 4, 5]) });
  t.send({ type: "post", record: b64(rec11) });
  assert.deepEqual(await t.next(), { type: "posted", id: idOf(rec11), good: 0 });
  t.send({ type: "post", record: b64(rec12) });
  assert.deepEqual(await t.next(), { type: "posted", id: idOf(rec12), good: 0 });
  records.set(11, rec11);
  records.set(12, rec12);
});

await check("mine returns the author's ten records out, newest first, across floors", async () => {
  t.send({ type: "mine", author: b64(X.key) });
  const frame = await t.next();
  assert.equal(frame.type, "mine");
  const serials = frame.records.map((entry) => serialOf(Buffer.from(entry.record, "base64")));
  assert.deepEqual(serials, [12, 11, 10, 9, 8, 7, 6, 5, 4, 3]);
  // each comes back as posted, the payload and signature included
  frame.records.forEach((entry, i) => assert.equal(entry.record, b64(records.get(12 - i)), `serial ${12 - i}`));
});

await check("the first two are gone from their floor's sample, which keeps at most 2 of a block", async () => {
  const fresh = connect(floorUrl(F1, 5));
  const sample = await fresh.next();
  const ids = sampleIds(sample);
  assert.ok(!ids.includes(idOf(records.get(1))), "serial 1 is retired");
  assert.ok(!ids.includes(idOf(records.get(2))), "serial 2 is retired");
  // F1 holds serials 3 to 10, all in block (0, 0): two of them at most
  assert.equal(ids.length, 2);
  fresh.close();
});

await check("a retired serial is never taken again", async () => {
  t.send({ type: "post", record: b64(records.get(2)) });
  assert.equal((await t.next()).code, "refused");
});

t.close();
console.log("all records checks passed");
