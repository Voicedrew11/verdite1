# Verdite online protocol

**Version 1, draft (2026-10-26).** The service (`online/service`) and the client
(`online/client`) both follow this page; a change here is a change to both, in one commit. A
change that an older client cannot follow raises the version.

Defined: the realtime tier's signaling (a room, its peers, the relay of WebRTC offers, answers
and candidates between them, and the list of public rooms) and records (messages on the ground:
their bytes, a floor's records, and the floor mesh that gossips them; `docs/MESSAGES_PLAN.md`).
Bloodstains come later (`docs/NETWORKING_PLAN.md`).

## Names

| name | form | what |
|---|---|---|
| game | `[a-z0-9]{1,16}` | which game, so one service serves every port: `kf1`, `kf2`, `kf3` |
| room | `[A-Za-z0-9_-]{1,64}` | a room within the game, chosen by the players (a summon sign, a code to share) |
| build | `[A-Za-z0-9._+-]{1,64}` | the client's build; peers in a room must have the same one |
| peer | 16 lowercase hex digits | a peer within the room, given by the service on joining; never reused within a room |
| public room | `P[A-HJ-NP-Z2-9]{7}` | a room that may be listed; the client draws the code at random |
| host name | `[A-Za-z0-9 _-]{1,16}` | how a listed room's host is shown |

## Joining a room

A client opens a WebSocket:

```
GET /v1/rooms/{game}/{room}?protocol=1&build={build}[&listed=1&host={name}]
Upgrade: websocket
```

With `listed=1`, a public room is listed (see "The list of public rooms") while the peer that asked
is its first, named by `host`. Asked by a later peer, `listed` is ignored. A room is never listed
for a peer that did not ask, so a code shared by friends stays off the list.

Limits are per address and approximate (counted per Cloudflare location). Refused before the upgrade with an HTTP status and a JSON body `{"error": code, "message": text}`:

| status | code | when |
|---|---|---|
| 400 | `bad_request` | a name does not match its form, or the request is not a WebSocket upgrade |
| 400 | `protocol` | the client's protocol version is not one the service speaks |
| 400 | `not_listable` | `listed` is given for a room that is not a public room, or `host` is malformed |
| 404 | `not_found` | any other path |
| 429 | `rate_limited` | too many joins from one address (30 a minute) |

Every frame after the upgrade is one JSON object with a `type`, as UTF-8 text. Binary frames and
frames over 64 KiB are an error. The first frame from the service is `welcome` or `error`.

## Service to client

| type | fields | meaning |
|---|---|---|
| `welcome` | `protocol`, `peer`, `peers` | you joined as `peer`; `peers` are already in the room, oldest first, so the first is the one in it longest (a game's host) |
| `join` | `peer` | a peer joined after you |
| `leave` | `peer` | a peer left |
| `signal` | `from`, `data` | a peer sent you `data` |
| `turn` | `iceServers`, `seconds` | the answer to your `turn`: the RTCIceServer list to connect with, good for `seconds` (1800). Empty when the service has no TURN key (local testing) |
| `error` | `code`, `message` | see below; `full`, `build_mismatch` and `unavailable` close the socket after it |

Error codes after the upgrade:

| code | when |
|---|---|
| `full` | the room has its most peers (4) |
| `build_mismatch` | the room's peers have another build |
| `bad_message` | a frame is not JSON, not an object, of an unknown type, binary or too long |
| `unknown_peer` | a `signal` names a peer not in the room |
| `rate_limited` | more than 50 frames in a second, or more than 10 `turn` requests a minute from one address (an IPv4 address or an IPv6 /64, as every limit counts them); the frame is dropped |
| `alone` | a `turn` request with no other peer in the room |
| `unavailable` | the service could not get a TURN credential, or is over its TURN budget for the month; try again later |

## Client to service

| type | fields | meaning |
|---|---|---|
| `signal` | `to`, `data` | send `data` to peer `to`; it arrives as `signal` with your id as `from` |
| `turn` | | ask for a TURN credential, answered by `turn` or an error. Only with another peer in the room. A peer keeps its last two: a new one revokes the one before the last |
| `describe` | `floor` | a listed room's host: the floor its list row shows, a whole number 0 to 99. Ignored from anyone else; at most one a second counts |

`data` is opaque to the service, a JSON object the clients agree on. Every one names `gen`, the
generation of the pair's connection it belongs to (a whole number, 0 for the first; see "Moving to
a new credential"):

| `data.kind` | fields | |
|---|---|---|
| `offer` | `gen`, `sdp` | the newer peer offers to each peer in its `welcome`'s `peers` |
| `answer` | `gen`, `sdp` | |
| `candidate` | `gen`, `candidate`, `mid` | a trickled ICE candidate |
| `restart` | `gen` | from the older peer: please offer a new generation, at least `gen` |

A candidate, in a `candidate` or in an SDP, gives an IP address, never a name: a client drops a signal
with a name, since looking it up would tell the name's DNS server about the player's resolver.

So the peer that joined later always offers, and two peers never offer to each other at once.
A generation is never reused between a pair, and signals for one that is gone are ignored.

Leaving is closing the socket. The service sends `leave` to the others and revokes the leaver's
TURN credential.

## The list of public rooms

```
GET /v1/lobby/{game}?protocol=1&build={build}
Upgrade: websocket
```

Refused before the upgrade as a join is (`bad_request`, `protocol`, `rate_limited`). The service
sends `rooms` at once and again whenever the list changes, at most once a second; the client sends
nothing, and leaves by closing the socket.

| type | fields | meaning |
|---|---|---|
| `rooms` | `rooms` | the listed rooms of this game and build, newest first, at most 50: each `{room, host, floor, players, most}` |

A room is on the list from its first peer's `welcome` until that peer leaves (a room whose host has
gone ends: the coop world is the host's). `players` counts its peers; a full room stays listed.

## Connections

- **Relay only.** Clients connect with the ICE transport policy `relay`, so the only candidates
  are Cloudflare's TURN addresses and no peer learns another's IP address. A client may allow host
  candidates when `iceServers` is empty, for testing on one machine.
- **Two data channels** per pair of peers, negotiated out of band (`negotiated`, created by both peers) with fixed
  ids: `0` reliable and ordered (`control`), `1` unreliable and unordered (`state`, no retransmits).
- **TURN credentials on request.** A client asks with `turn` when it is about to connect to a
  peer (on its `welcome` with peers, or on a `join`), not before. A credential lasts half an hour
  (`seconds`); a client asks again halfway through it, or 15 minutes before its end if sooner.

## Moving to a new credential

A connection keeps the TURN credentials it was made with, and the relay refuses its next refresh
(every few minutes) once one of them is revoked or expired: measured on the deployed service,
2026-10-26, with 2-minute credentials, data stopped at the first refresh 4.5 minutes in. So when a
client gets a new credential, each of its connections moves to a new one made with it, and the
service keeps the previous credential alive until the move is done (it revokes only the one
before that, or both when the peer leaves).

1. The newer peer offers generation `gen` + 1 (the older one asks it to with `restart`). The pair
   connects it alongside the current one, with the same two channels.
2. When the new connection's channels are open, each side sends the text message `moved` on the
   current connection's `control` channel, after the last packet it sends there, and from then on
   sends everything on the new connection.
3. A side delivers `control` packets from the current connection until the peer's `moved`, holding
   those from the new connection, then delivers what it held and carries on from the new one. It
   answers `moved` with the text message `seen`.
4. A side closes the old connection once it has sent `moved`, received the peer's `moved`, and
   received `seen` for its own. A peer closing it after its own `moved` and ours is a finish too
   (its `seen` can be lost in the close; nothing else can).

`state` packets go on whichever connection is current for the sender; they are unordered anyway.
A move whose new connection does not open within 30 seconds is dropped and tried again; one that
fails after a side has sent `moved` loses the peer. Text messages on `control` are only these
markers: the game's packets are binary.
- **The budget.** Checked every minute (`src/quota.ts`). An address (an IPv4 address, or an IPv6 /64)
  over its quota, 100 MB in a minute or 10 GB in a day, has its credentials revoked and is refused
  for a week. A day past three times its even share of the month's budget (`TURN_BUDGET_GB`, 500 by
  default), or the budget itself, stops new connections; a peer that already holds a credential can
  still renew. Past the hard cap (`TURN_HARD_GB`, 900) every credential is revoked. All refusals are
  `unavailable`.

## Records

A record is something a player leaves in a game's world, such as a message on the ground. The
service and the client know no game: a record is an **area** (a floor), a **place** in it and a
**payload** of at most 8 bytes, each the game's.

### Bytes

Little-endian. The signed body is 54 bytes:

| offset | bytes | field |
|---|---|---|
| 0 | 1 | version, 1 |
| 1 | 4 | area |
| 5 | 4 | place |
| 9 | 1 | payload size, 0 to 8 |
| 10 | 8 | payload, zero past its size |
| 18 | 32 | author: an Ed25519 public key, one per install |
| 50 | 4 | serial: the author's count of records signed, from 1 |

The record is the body followed by a 64-byte Ed25519 signature over the UTF-8 domain
`verdite-record/{game}`, one zero byte, and the body: 118 bytes. Its **id** is the first 8 bytes
of the SHA-256 of the body, read little-endian, with 0 made 1; in JSON, 16 lowercase hex digits
of that number.

**Retirement**: an author has at most 10 records out. A record of serial s retires that author's
records of serial s - 10 or less, wherever they are; a retired record is never taken again.

A **rating** says a record is good: the record's id (8 bytes, little-endian), the rater's key
(32), and a signature (64) over `verdite-rating/{game}`, a zero byte and those 40 bytes: 104
bytes. One per rater and record counts; a rating of one's own record does not.

In JSON frames, records and ratings are standard base64 with padding.

### A floor's records

```
GET /v1/floors/{game}/{area}/records?protocol=1&seed={0..4294967295}
Upgrade: websocket
```

`area` is a whole number 0 to 4294967295. Refused before the upgrade as a join is. The service
sends `records` at once; then the client may send:

| client sends | fields | service answers |
|---|---|---|
| `post` | `record` | `posted` with `id` and `good` 0, or `error` `refused` (bad bytes or signature, another area than the socket's, a serial not above the author's newest, or rate-limited: 10 posts a minute per address) |
| `rate` | `rating` | `rated` with `id` and `good`; `error` `refused` for a bad signature, an unknown record, one's own, or rate-limited: 30 ratings a minute per address |
| `mine` | `author` (base64 key) | `mine` with `records`: that author's records out, in any area, newest first; `error` `rate_limited` past 60 a minute per address |

| service sends | fields | meaning |
|---|---|---|
| `records` | `records` | the floor's sample (below): each `{record, good}`, at most 24 |
| `posted`, `rated` | `id`, `good` | |
| `mine` | `records` | each `{record, good}` |
| `error` | `code`, `message` | as for rooms, plus `refused`; none closes the socket |

The service answers a socket's requests in the order they came, one answer each (`posted`,
`rated`, `mine` or `error`), so a client matches an `error` to its oldest unanswered request.

**The sample** is drawn as the client shows records (`docs/MESSAGES_PLAN.md`, "The fraction a
player sees"): the area's records in blocks of 8 by 8 cells (the place's low byte is the cell x,
its second byte the cell z), at most 2 a block and 24 in all, without replacement, weighted by
1 + good, from `seed`.

### The floor mesh

```
GET /v1/floors/{game}/{area}/mesh?protocol=1&build={build}
Upgrade: websocket
```

The room protocol above (welcome, join, leave, signal, turn, the generations of a connection and
the moves to a new credential), with these differences:

- **No limit on players and no build check**: the record format is the same in every build.
- **`welcome.peers` is at most 3 random players already there with fewer than 4 links**, and only
  those get `join`. The newcomer offers to them, as in a room.
- **`leave` goes only to the leaver's links.** A player left with fewer than 2 links is given new
  ones: the service picks a player with fewer than 4 links not already linked to it and sends the
  newer of the two `link` with the other's `peer` (offer to it, as to a peer in `welcome`) and the
  older `join` with the newer's `peer`.
- **`signal` only to a linked peer**, else `unknown_peer`.
- `describe` is ignored.

On a mesh connection's `control` channel the clients gossip records, each packet binary:

| byte 0 | byte 1 | then |
|---|---|---|
| 1 (version) | 1: **shown** | a count (1 byte), then that many records (118 bytes each): what the sender shows, sent once when the connection opens; a second is malformed |
| 1 | 2: **new** | hops left (1 byte, at most 3), then one record: a new post; a receiver that did not hold it forwards it to its other links with one hop fewer, while hops are left |

A receiver checks every record (bytes, signature, retirement) before keeping or forwarding it,
and ignores everything more from a peer that sent a malformed or forged one. A receiver keeps at most
256 records from gossip in an area (the service's sample and its own posts still go in), since anyone
can sign as many as they like with new keys; past that it drops what gossip brings and forwards nothing.
