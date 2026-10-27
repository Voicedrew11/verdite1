# Coop plan

**Draft, 2026-10-26, not yet reviewed.** Nothing here is built except the other players' knight
([PLAYER_MODEL.md](PLAYER_MODEL.md)). When a section changes, edit it and add a line to "Changes"
at the end rather than leaving the old text standing. Transport, rooms and the service are
[NETWORKING_PLAN.md](NETWORKING_PLAN.md)'s.

The goal: **one seamless playthrough together**, start to end. Both players are equals in one
world, either can be anywhere in it, including another floor, and each keeps what they find.

## Fixed

- **One tick rate per room, the host's.** The game counts updates, not time, so the rate is its
  speed; players at different rates would walk and swing at different speeds. Joining a room
  sets the guest's `gameplay.tick_rate` to the host's for the session.
- **Not a summon.** Joining the host's floor with the guest's own world set aside (Dark Souls)
  starves the guest of items and makes them a visitor.

## Rejected

| model | what | why not |
|---|---|---|
| summon | the guest joins the host's world while there | item shortage; not a playthrough together |
| lockstep (and rollback) | every machine simulates the whole world from everyone's inputs | the game holds one floor in memory, so both players would be held to the same floor; also input delay, the slowest player sets the pace, menus pause everyone, a desync is fatal and every platform must be bit-identical |
| host-authoritative with prediction | the host simulates everyone, the guest predicts and is corrected | built for not trusting clients, which coop does not need; the guest's movement would be re-run against the host's collision and corrected |

## The model: authority goes with the floor

- **Each player owns their character:** movement, inventory, level, equipment, applied at once on
  their own machine. Others see it as a puppet: poses numbered by game update, buffered and
  interpolated, with actions (swings, spells, deaths) sent as events and animated locally at the
  game's rate.
- **Whoever is on a floor runs it.** Each machine already simulates its player's floor, so it is
  in charge of that floor's enemies, doors and events. On different floors nothing conflicts.
- **Two on a floor: one runs it** (see Questions). The other is sent a snapshot on arrival
  (enemies' positions, HP and AI state, doors) and from then on draws those enemies as puppets,
  as it draws the other player. When the owner leaves, ownership passes to someone still there,
  with a fresh snapshot.
- **Hits are decided by whoever feels them.** A player's hit on an enemy is judged on their own
  screen and sent to the floor's owner, which applies it. An enemy's hit on a player is judged on
  that player's machine, so no one takes a hit they dodged on their screen.
- **The lasting world is a shared record:** doors opened, bosses killed, levers, story flags.
  Each flag only goes from unset to set, so two players' records merge by union with no
  conflicts. Both machines hold a copy, and both saves; whoever runs a floor loads it with the
  merged flags.

## Items

- **Chests and floor items are per player.** Opening a chest gives its contents and marks it
  opened for that player; the partner still finds it full. This is the one part of the world that
  is not shared.
- **What changes the world is shared:** a door unlocked with a key opens for both.
- **Key items** need a rule, or a player who lacks one is stuck later on their own (see
  Questions).

## What else changes

- **Menus do not pause the world in a room.** With a partner on the floor, opening the inventory
  would freeze their enemies; in a room only the player's view stops. A change to the game's
  behaviour, but coop cannot work without it.
- **Saves:** each player's save holds their character and the shared record. Playing alone
  between sessions is allowed; meeting again merges the records.
- **The pose** gains the game update number and the height (on a stair or in the air the knight
  is drawn on the cell's floor today).

## Order

1. The smooth knight: poses numbered by update, a playback buffer, swings as events, height.
   Needed by this model whatever else changes.
2. The shared record: which game flags are world state, which are per player (chests), merged
   by union, carried in the save.
3. Floor ownership: the snapshot on arrival, enemies drawn as puppets, the handover.
4. Hits across machines; menus that do not pause; kill credit.

## Questions

1. **Key items:** given to both players when one picks one up, or kept by whoever picks it up?
2. **Experience:** to whoever lands the last hit, or shared by everyone on the floor?
3. **Who runs a shared floor:** the first to arrive, or always the host? Leaning first to
   arrive, which avoids a handover every time the host walks in.

## Weak points (to argue with)

- **Is per-player loot faithful?** The original never had to decide it, and two copies of a
  unique weapon change the game's economy: close to a modern convenience.
- **Enemies that respawn, or that were killed by one player while the other was away:** is a kill
  world state (shared) or per player, and which of the game's flags say so?
- **Handover mid-fight:** the owner walks off the floor while enemies are chasing the other
  player; the snapshot must carry AI state well enough that they do not reset or jump.
- **The record merges by union only if every flag only ever goes one way.** Any flag the game
  clears (a door that closes again, a lever that toggles) breaks that and needs its own rule.
- **Trust:** hits judged by whoever feels them let a modified client never be hit. Fine between
  friends; not if rooms are ever public.
- **Save divergence:** two saves of one world that play apart for a while and meet again: is
  union always the right merge, and what about story order (a flag that only makes sense after
  another)?

## Changes

- 2026-10-26: first draft, from the discussion that ruled out the summon model and lockstep.
