# Networking plan

**Draft, 2026-10-22. Subject to change** as considerations come up; when one
changes the plan, edit the section and add a line to "Changes" at the end
rather than leaving the old text standing. Built so far: the signaling service
(`online/service`), its protocol (`online/protocol/PROTOCOL.md`) and the client library
(`online/client`, interface `include/verdite_online.h`), tested in pairs through the deployed
service; no game uses it yet.

The goal: Demon's Souls-style online play in the Verdite ports, namely
**messages** left on the ground, **bloodstains** that replay another player's
last seconds, and **coop** (summoning another player into your world), on
Cloudflare Realtime's TURN service with a gossip layer between players. It is
meant for every Verdite game, not only KF1.

## Four layers, three repos

| layer | what | where |
|---|---|---|
| **service** | Cloudflare Workers and Durable Objects: TURN credentials, signaling, rooms, storage, ratings, moderation | its own repo, deployed once for all games |
| **client library** (`verdite-online`) | transport, gossip, identity, records, sessions; a narrow C interface | its own repo, separate from the core |
| **core glue** | overlay screens, tick-state hooks, settings | the C++ Verdite core, depending on the client library *optionally* |
| **game layer** | message vocabulary, bloodstain recording and ghosts, phantoms, coop sync | each game: `game/` and `patches/` |

The service and the client library share one protocol definition, so they
live together (one networking repo) or side by side. The repo name
`verdite-online` is a placeholder. **For now both live in this repo, under
`online/`** (`online/README.md`): `protocol/`, `service/` and `client/`, none
depending on `libverdite/` or the game, so they move out unchanged once the
protocol settles.

### Why the client library is not in the core

- **Different lifecycle.** The core changes with each port's needs; the
  networking code changes with the protocol and must stay wire-compatible
  across every game and build at once.
- **Different consumers.** The C# recompilations (Verdite2, Verdite3) could
  use the client library through P/Invoke, but never the C++ core.
- **Heavy, optional dependencies.** libdatachannel, TLS and the WebRTC stack
  should not be in every build of the core; the 3DS wants only HTTPS.

The games take it in the same way as the core (a subtree, each game pinning its
own commit), not as a read-only submodule like the decomp: we edit it.

## The service

- **TURN credentials.** Cloudflare's TURN key cannot ship in a client, so a
  Worker hands out short-lived credentials.
- **Signaling and rooms** (Durable Objects): matchmaking, summon signs, the
  WebRTC offer/answer exchange.
- **Storage** of messages and bloodstains (D1 or R2, with a Durable Object per
  area as a cache). Gossip spreads records between online players, but cannot
  keep them alive while nobody is playing, so a persistent store sits under it.
- **Ratings, rate limits, moderation.** Template-only messages, as in Demon's
  Souls, are most of the moderation: no free text.
- **Keys namespaced by game**, so one backend serves KF1, KF2, KF3 and later
  ports, and the recompiled builds could join if the wire format stays
  language-neutral.

## The client library

- **Transport**: WebRTC data channels through libdatachannel (small, C++,
  supports TURN). **Relay-only** through Cloudflare TURN, so peers never see
  each other's IP addresses.
- **Gossip**: membership and dissemination of records among connected peers;
  the signaling client.
- **Identity**: a key per install; records are signed, so messages and
  bloodstains cannot be forged in another player's name.
- **Records API**: post and fetch by (game, area, position), with ratings. A
  bloodstain is a record whose payload the library does not interpret.
- **Sessions**: join, leave, presence, clock agreement, carrying tick state
  between peers.
- **A C interface**, so C++ ports link it directly and C# builds call it
  through P/Invoke. A thin helper process around the same interface stays
  possible later on desktop, if isolation ever matters more than simplicity;
  nothing in the design depends on it.

## Platform tiers

| tier | features | transport | platforms |
|---|---|---|---|
| **asynchronous** | messages, bloodstains | HTTPS to the Worker | all, 3DS included |
| **realtime** | coop, live gossip | WebRTC through TURN | Windows, Linux, macOS, Android; Switch probably |

The 3DS has no WebRTC stack, so it gets the asynchronous tier only. Switch
homebrew needs checking.

## The game layer

King's Field has no netcode at all, so this is most of the work, and it is per
game:

- **Message vocabulary**: per-game nouns (items, enemies, places) and where a
  message can be placed, in game coordinates and area ids.
- **Bloodstains**: the player's pose recorded each tick for the last few
  seconds, and played back as a ghost drawn with the game's own model.
- **A remote player in the world**: patches so that actors, collision and
  damage know a second player exists.
- **Coop sync**: someone is authoritative for enemies, damage, doors, events,
  area changes and saves.
  - **Host-authoritative**: the host runs the world, the guest sends inputs
    and position. The robust default.
  - **Deterministic lockstep**: both machines simulate from the same inputs.
    Possibly viable, because the game logic is integer fixed point at a fixed
    20 updates a second, but only if the port is fully deterministic. The
    fuzzer's replay tests that: one input recording, run twice, must give the
    same game memory tick for tick.

## Order and dependencies

The groundwork is the same as the fuzzer's and comes first anyway
(`docs/verdite-port.md`, "Order"):

1. Input recording and deterministic replay. A bloodstain is a short
   recording; lockstep needs determinism proven.
2. Headless runs, so two instances can run on one machine to test coop.
3. The asynchronous tier: service storage, the records API, messages, then
   bloodstains.
4. The realtime tier: TURN, signaling, gossip, then coop.

From the start, keep the wire protocol out of the game layer, so KF2 and KF3
get online play by writing only their own game layer.

## Principles

- **Opt-in**, like Demon's Souls' offline mode; the game played alone is
  unchanged.
- **Template messages only**; no free text.
- **Relay-only connections**; no player's IP address reaches another player.
- **Signed records**; the service verifies, and rate-limits per key.

## Open questions

- Host-authoritative or lockstep for coop: decided by the determinism test.
- What a bloodstain records (pose only, or pose plus inputs) and how long.
- Whether coop needs the host and guest on the same build, and how versions
  are matched.
- Whether Switch homebrew can run libdatachannel.
- What the service costs at expected traffic, and who pays.
- Whether messages and bloodstains are shared across regions and translations
  (JP and US builds of one game, the English fan translation).

## Changes

- 2026-10-22: first draft, from the discussion that set the four layers and
  the separate repo.
- 2026-10-26: the service and the client start in this repo, under `online/`,
  until the protocol settles. The realtime tier's signaling and TURN come
  first, ahead of the order above, since exact replay (step 1) is done: rooms
  of up to 4 peers of one build, the later peer offering, relay-only, the TURN
  credential issued per peer for 6 hours and revoked when it leaves.
- 2026-10-26: the client library is built on libdatachannel (libjuice, so TURN over UDP only).
  Its interface gained who joined first (`older`, for choosing a host), CONTROL packets up to
  64 KiB (a world's state on joining), a CONTROL queue limit that fails a slow peer, and
  `vo_link_of` (round trip, queued bytes). The client renews its one credential before it runs
  out; whether a renewal's revocation, or an expiry, ends connections already using the old one
  is to be measured with `TURN_CREDENTIAL_SECONDS`.
- 2026-10-26: measured: it does. With 2-minute credentials on the deployed service, a pair's data
  stopped at the relay's first refresh after the revocation, 4.5 minutes in. A connection now
  moves to a new one at each renewal, with a handover that keeps CONTROL exactly once and in
  order (`online/protocol/PROTOCOL.md`, "Moving to a new credential"), and the service keeps a
  peer's previous credential until the next, so sessions have no length limit. Checked on the
  deployed service with 2-minute credentials: 12 minutes relayed through TURN, 11 moves, all
  14,365 CONTROL packets each way in order, no peer lost.
- 2026-10-26: other players in the room appear in the world, as a **stand-in** to be replaced
  by a model of the player's own: each borrows a player-sized enemy or NPC model of the current
  floor (`src/game/online_player_render.cpp` in `patches/src/game.patch`, a table in order of preference, matched by
  vertex count since floor 5's variants put other models in the same slots): an enemy 1750 tall
  on floors 1 to 3 (334 vertices), others on floors 4 and 5. Only the current floor's models can
  be borrowed, since another floor's textures are not in VRAM. The model faces where the
  player's camera looks: the camera walks along (-sin h, cos h), actors and map events along
  (-sin a, -cos a), so a = half turn - h; checked on two headless instances through a quarter
  turn. It is drawn in its rest pose: the animation cache's records name their owner's slot, and
  a stand-in has none that outlives a game reset. The pose (floor, x, z, yaw) has no height, so
  a player on a stair or in the air is drawn on the cell's floor.
- 2026-10-26: the stand-in is replaced by a knight of our own in the game's style, holding the
  player's real weapon: the game's first-person weapon model (a gauntlet with the weapon) on the
  knight's elbow, posed by its clip at the attack phase the pose now carries (version 2, 17
  bytes: weapon and phase). [PLAYER_MODEL.md](PLAYER_MODEL.md) has the model, its tools and
  what is left (walking, smoother swings, height).
