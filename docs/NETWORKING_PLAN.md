# Networking plan

**Draft, 2026-10-22. Subject to change** as considerations come up; when one
changes the plan, edit the section and add a line to "Changes" at the end
rather than leaving the old text standing. Nothing here is built yet.

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
`verdite-online` is a placeholder.

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
