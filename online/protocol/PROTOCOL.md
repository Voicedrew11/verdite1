# Verdite online protocol

**Version 1, draft (2026-10-26).** The service (`online/service`) and the client
(`online/client`) both follow this page; a change here is a change to both, in one commit. A
change that an older client cannot follow raises the version.

Only the realtime tier's signaling is defined yet: a room, its peers, and the relay of WebRTC
offers, answers and candidates between them. Records (messages, bloodstains), identity and
signing come later (`docs/NETWORKING_PLAN.md`).

## Names

| name | form | what |
|---|---|---|
| game | `[a-z0-9]{1,16}` | which game, so one service serves every port: `kf1`, `kf2`, `kf3` |
| room | `[A-Za-z0-9_-]{1,64}` | a room within the game, chosen by the players (a summon sign, a code to share) |
| build | `[A-Za-z0-9._+-]{1,64}` | the client's build; peers in a room must have the same one |
| peer | 16 lowercase hex digits | a peer within the room, given by the service on joining; never reused within a room |

## Joining a room

A client opens a WebSocket:

```
GET /v1/rooms/{game}/{room}?protocol=1&build={build}
Upgrade: websocket
```

Limits are per address and approximate (counted per Cloudflare location). Refused before the upgrade with an HTTP status and a JSON body `{"error": code, "message": text}`:

| status | code | when |
|---|---|---|
| 400 | `bad_request` | a name does not match its form, or the request is not a WebSocket upgrade |
| 400 | `protocol` | the client's protocol version is not one the service speaks |
| 404 | `not_found` | any other path |
| 429 | `rate_limited` | too many joins from one address (30 a minute) |

Every frame after the upgrade is one JSON object with a `type`, as UTF-8 text. Binary frames and
frames over 64 KiB are an error. The first frame from the service is `welcome` or `error`.

## Service to client

| type | fields | meaning |
|---|---|---|
| `welcome` | `protocol`, `peer`, `peers` | you joined as `peer`; `peers` are already in the room |
| `join` | `peer` | a peer joined after you |
| `leave` | `peer` | a peer left |
| `signal` | `from`, `data` | a peer sent you `data` |
| `turn` | `iceServers`, `seconds` | the answer to your `turn`: the RTCIceServer list to connect with, good for `seconds` (3600). Empty when the service has no TURN key (local testing) |
| `error` | `code`, `message` | see below; `full`, `build_mismatch` and `unavailable` close the socket after it |

Error codes after the upgrade:

| code | when |
|---|---|
| `full` | the room has its most peers (4) |
| `build_mismatch` | the room's peers have another build |
| `bad_message` | a frame is not JSON, not an object, of an unknown type, binary or too long |
| `unknown_peer` | a `signal` names a peer not in the room |
| `rate_limited` | more than 50 frames in a second, or more than 10 `turn` requests a minute from one address; the frame is dropped |
| `alone` | a `turn` request with no other peer in the room |
| `unavailable` | the service could not get a TURN credential, or is over its TURN budget for the month; try again later |

## Client to service

| type | fields | meaning |
|---|---|---|
| `signal` | `to`, `data` | send `data` to peer `to`; it arrives as `signal` with your id as `from` |
| `turn` | | ask for a TURN credential, answered by `turn` or an error. Only with another peer in the room; a new one revokes your last |

`data` is opaque to the service, a JSON object the clients agree on:

| `data.kind` | fields | |
|---|---|---|
| `offer` | `sdp` | the newer peer offers to each peer in its `welcome`'s `peers` |
| `answer` | `sdp` | |
| `candidate` | `candidate`, `mid` | a trickled ICE candidate |

So the peer that joined later always offers, and two peers never offer to each other at once.

Leaving is closing the socket. The service sends `leave` to the others and revokes the leaver's
TURN credential.

## Connections

- **Relay only.** Clients connect with the ICE transport policy `relay`, so the only candidates
  are Cloudflare's TURN addresses and no peer learns another's IP address. A client may allow host
  candidates when `iceServers` is empty, for testing on one machine.
- **Two data channels** per pair of peers, negotiated out of band (`negotiated`, created by both peers) with fixed
  ids: `0` reliable and ordered (`control`), `1` unreliable and unordered (`state`, no retransmits).
- **TURN credentials on request.** A client asks with `turn` when it is about to connect to a
  peer (on its `welcome` with peers, or on a `join`), not before. A credential lasts an hour; a
  longer session asks again before it runs out, and the old one is revoked.
- **The budget.** The service stops issuing credentials once the month's TURN traffic passes its
  budget (`TURN_BUDGET_GB`, 500 by default), checked every 10 minutes.
