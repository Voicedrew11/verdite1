// The TURN quotas' decisions, without a service: node test/quota.mjs
import assert from "node:assert/strict";
import {
  ADDRESS_DAY_BYTES,
  ADDRESS_LIVE_CREDENTIALS,
  ADDRESS_MINUTE_BYTES,
  addressKey,
  dayAllowanceBytes,
  judgeUsage,
  turnVerdict,
} from "../src/quota.ts";

const GB = 1e9;

// One customer's IPv6 block is one address, however it is written.
assert.equal(addressKey("203.0.113.9"), "203.0.113.9");
assert.equal(addressKey("2001:db8:1:2:aaaa::1"), "2001:db8:1:2::/64");
assert.equal(addressKey("2001:db8:1:2:bbbb:cccc:dddd:eeee"), "2001:db8:1:2::/64");
assert.equal(addressKey("2001:0DB8:0001:0002::5"), "2001:db8:1:2::/64");
assert.equal(addressKey("2001:db8::1"), "2001:db8:0:0::/64");
assert.equal(addressKey("::1"), "0:0:0:0::/64");
assert.equal(addressKey("::ffff:203.0.113.9"), "203.0.113.9");
assert.notEqual(addressKey("2001:db8:1:2::1"), addressKey("2001:db8:1:3::1"));

// A burnt day takes a share of what is left, so something is always left for tomorrow.
const first = new Date("2026-11-01T12:00:00Z");
const last = new Date("2026-11-30T12:00:00Z");
assert.equal(dayAllowanceBytes(300 * GB, 0, 0, first), 30 * GB);
assert.equal(dayAllowanceBytes(300 * GB, 290 * GB, 0, last), 30 * GB);
assert.equal(dayAllowanceBytes(300 * GB, 40 * GB, 10 * GB, first), 27 * GB, "today's own use is not taken off today's share");
let left = 500 * GB;
for (let day = 1; day <= 27; day++) {
  const now = new Date(Date.UTC(2026, 10, day, 12));
  left -= dayAllowanceBytes(left, 0, 0, now);
  assert.ok(left > 0, `a month of an attack must leave something on day ${day}`);
}

const asked = {
  month_bytes: 0,
  today_bytes: 0,
  blocked: false,
  live_credentials: 0,
  renewing: false,
  budget_bytes: 500 * GB,
  hard_bytes: 900 * GB,
  now: first,
};
assert.equal(turnVerdict(asked), "ok");
assert.equal(turnVerdict({ ...asked, blocked: true, renewing: true }), "address", "a blocked address never renews");
assert.equal(turnVerdict({ ...asked, live_credentials: ADDRESS_LIVE_CREDENTIALS }), "address");
assert.equal(turnVerdict({ ...asked, month_bytes: 500 * GB, today_bytes: 0 }), "budget");
assert.equal(turnVerdict({ ...asked, month_bytes: 500 * GB, renewing: true }), "ok", "a game in progress goes on");
assert.equal(turnVerdict({ ...asked, month_bytes: 900 * GB, renewing: true }), "budget", "the hard cap ends everything");
const burnt = { ...asked, month_bytes: 60 * GB, today_bytes: 60 * GB };
assert.equal(turnVerdict(burnt), "budget", "a day past its share stops new connections");
assert.equal(turnVerdict({ ...burnt, renewing: true }), "ok");

// Usage adds up by address, across credentials and against the last minute.
const owners = new Map([["u1", "a"], ["u2", "a"], ["u3", "b"]]);
const quiet = judgeUsage(owners, new Map([["u1", 1000], ["u3", 2000]]), {}, 1);
assert.deepEqual(quiet.over, []);
assert.deepEqual(quiet.totals, { a: 1000, b: 2000 });
assert.deepEqual(
  judgeUsage(owners, new Map([["u1", ADDRESS_DAY_BYTES / 2], ["u2", ADDRESS_DAY_BYTES / 2]]), { a: ADDRESS_DAY_BYTES - 1 }, 1).over,
  ["a"],
  "over the day, across its credentials",
);
assert.deepEqual(
  judgeUsage(owners, new Map([["u3", 5 * ADDRESS_MINUTE_BYTES]]), { b: 4 * ADDRESS_MINUTE_BYTES }, 1).over,
  ["b"],
  "over the minute",
);
assert.deepEqual(
  judgeUsage(owners, new Map([["u3", 5 * ADDRESS_MINUTE_BYTES]]), { b: 4.5 * ADDRESS_MINUTE_BYTES }, 1).over,
  [],
  "steady use under the minute's limit",
);
assert.deepEqual(judgeUsage(owners, new Map([["old", ADDRESS_DAY_BYTES]]), {}, 1).over, ["unknown:old"]);
// A reading after a gap, or the first of a day, is not a burst: it covers more than a minute.
assert.deepEqual(judgeUsage(owners, new Map([["u3", 5 * ADDRESS_MINUTE_BYTES]]), {}, 600).over, []);
assert.deepEqual(judgeUsage(owners, new Map([["u3", 5 * ADDRESS_MINUTE_BYTES]]), { b: 0 }, 4).over, ["b"]);
console.log("ok   quotas");
