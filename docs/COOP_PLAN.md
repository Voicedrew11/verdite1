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
- **The coop world is the host's save,** separate from their single-player saves. It holds the
  world and a character slot for each guest; a guest's character lives there and nowhere else
  (the guest's machine keeps a backup copy). The host may play it alone between sessions; a guest
  only plays it in the host's room.
- **Late joiners start fresh:** level 1 and the starting gear (where they start is Question 3).
  Chests and drops are per player, so every floor behind them is still full for them, and the
  experience rule brings them up quickly on a deep floor. No hand-made catch-up.
- **Experience:** every player on the floor gets a kill's full experience.
- **Death, per character** (`player_death_restart`). With the revival flag set and a Dragon King
  Grass Fruit, the player revives on floor 1 keeping everything, as in single player. Without one,
  the game calls `game_state_initialize()` (`player_death.cpp`), which is a new game: level 1,
  starting gear, the world's flags and the shops reset. Coop cannot do that to a shared world, so
  in a room the dead player's slot goes back to its last save (the last restore point), losing
  what the character gained since. **This is a departure from the game, and softer than it**,
  taken because the alternative wipes the partner's world. The world keeps its progress and the
  partner loses nothing. A boss killed since that save stays dead, so its drop is lost to the dead
  player as to one who missed the kill (see Weak points).

## Rejected

| model | what | why not |
|---|---|---|
| summon | the guest joins the host's world while there | item shortage; not a playthrough together |
| lockstep (and rollback) | every machine simulates the whole world from everyone's inputs | the game holds one floor in memory, so both players would be held to the same floor; also input delay, the slowest player sets the pace, menus pause everyone, a desync is fatal and every platform must be bit-identical |
| host-authoritative with prediction | the host simulates everyone, the guest predicts and is corrected | built for not trusting clients, which coop does not need; the guest's movement would be re-run against the host's collision and corrected |

## The model: authority goes with the cause

The floor code knows one player, and not only in the enemies: floor scripts read the player's cell
and facing (`map_scripts.cpp`: the floor-3 healing tile, the floor-5 boss trigger), traps fire from
the object pool (`map_object_pool.cpp`), the collision grid counts one occupant
(`collision.cpp`, `collision_adjust_cell_occupancy`) and enemy projectiles test one camera
(`effect_update.cpp`). A machine that ran a whole floor for two players would show the partner a
floor that ignores them. So authority is split by what causes a thing, not by floor:

- **Each player owns their character:** movement, inventory, level, equipment, applied at once on
  their own machine. Others see it as a puppet: poses numbered by game update, buffered and
  interpolated, with actions (swings, spells, deaths) sent as events and animated locally at the
  game's rate.
- **What a player triggers runs on that player's machine, against that player:** cell scripts
  (the healing tile, the boss trigger), pickups and chests, talking to an NPC and its exchanges,
  switches, doors opened by hand, restore points. These already read `player_state`, which on the
  triggering machine is the right player, so they run unchanged. What they change in the world (a
  flag, a door's link, an event's stage) is sent as a write to the floor's owner.
- **What happens on its own is run by the floor's owner:** enemies, trap emitters, moving objects,
  timers. With one player on a floor, that player's machine is the owner and nothing changes.
- **Two on a floor: one owns it** (see Questions). The other is sent a snapshot on arrival
  (enemies' positions, HP and AI state, emitters, doors) and from then on draws those enemies as
  puppets, as it draws the other player, while still running its own player's triggers. When the
  owner leaves, ownership passes to someone still there, with a fresh snapshot.
- **Enemies choose among the players on the floor.** The AI reads `actor_state.player_position`
  (`actor_behavior.cpp`), spawning near the player checks only `player_state`, and enemy
  projectiles are tested against `player_state.camera_position` (`effect_update.cpp`). On the
  owner's machine every one of these becomes a choice among the players on the floor: each actor
  targets the nearest, spawning keeps clear of all of them, and a projectile that could reach a
  remote player is sent to that player's machine to judge. Remote players are counted in the
  owner's collision grid, so enemies do not walk through them.
- **Hits are decided by whoever feels them.** A player's hit on an enemy is judged on their own
  screen and sent to the floor's owner, which applies it. An enemy's melee hit is decided inside
  its AI on the attack frame (`actor_try_attack_player`, `actor.cpp`: distance, angle, a random
  status roll), so on a remote target the owner sends an attack event (actor, attack parameters,
  update number) instead, and the victim's machine runs the same test against its own position
  and the puppet's, so no one takes a hit they dodged on their screen.
- **The lasting world is the host's save.** The game keeps a few story flags and a snapshot of
  each of its 5 floors (`KfMapSavedWorld`, `include/kf/lib/map.h`). The flags are script structs
  for floors 1, 3 and 5: `passage_opened`, `revival_enabled`, the floor-3 `revealed_piece_count`,
  and floor 5's `character_arrived`, `weapon_transformed`, `boss_encounter_started` and
  `boss_defeat`. The snapshot (`map_world_state_persist`, written when a floor is left) holds
  every event's state and dialogue stage, which persistent actors are dead, each object slot's id
  (a taken item becomes none), door and container links, and gold and items dropped on the
  floor. Enemies that respawn are not in it.
- **The coop save is on every machine.** The floor's owner is the only writer of that floor's
  world records and script flags; a triggering player's world changes reach them through it. Each
  write carries a version number and goes to every player. The host's copy is the one kept; a
  guest's copy is a backup that keeps the writes the host never received, merged into the host's
  copy at the next session. It is not playable on its own (what happens when the host drops is
  Question 4). Key spends are not a floor's: they go through the host, the first claim wins and a
  door refuses the second.
- **The game's own snapshot writes are guarded.** `map_world_state_persist` runs on revival
  (`player_death.cpp`), at a restore point (`map_scripts.cpp`) and on leaving a floor
  (`player_update.cpp`, `map_events.cpp`). On a machine that does not own the floor, those calls
  write only its player's part; otherwise the partner's stale copy would overwrite the owner's.
- **Each floor's snapshot splits in two:** the world's part (events, dead persistent actors,
  doors) and each player's part (item slots and drops, see Items).
- **NPC progress comes from the world, not the loader.** On loading a floor the game sets each
  NPC's dialogue stage to the loading player's `highest_floor`, capped by the NPC's limit, and can
  lower it (`map_event_refresh_dialogue_stage`, `map_event.cpp`); `map_load.cpp` also disables the
  floor-2 departing event by it. A level-1 guest who owns floor 1 would rewind the host's NPCs and
  the versioned write would carry the rewind into the host's save. In a room these read the
  world's highest floor (the highest any player has reached, kept in the coop save) and a stage
  is never written lower.

## The state map

Every field of the save (`SavedGameState`, `save_system.cpp`) is the world's or a player's:

| field | whose | |
|---|---|---|
| `player` (`KfPlayerState`) | player | level, vitals, equipment, position, current floor |
| `player.progress_state.highest_floor` | both | the player's own for warps; the world keeps the highest of all players for NPC stages and the floor-2 departing event |
| `world.floor1`, `floor3`, `floor5` | world | the story flags above |
| `world.floors[n].records` | split | events, dead persistent actors, doors: world; object slots and drops: player |
| `stock`, player bank | player | the inventory |
| `stock`, the two shop banks | see Question 2 | limited stock (the second shop holds one Gold Cross) |
| `learned` (`KfMagicLearningState`) | player | magic learned |

## Items

- **Chests and floor items are per player.** Opening a chest gives its contents and marks it
  opened for that player; the partner still finds it full.
- **Items from anything but a chest are per player too:** an NPC's gift or a boss's drop goes to
  every player in the room; an enemy's drop appears for each player on the floor. No item races.
- **Key items are held by the world** (ids 52 to 76: Gold Cross, the keys, the seal stones,
  Demon King's Hand and so on). Once anyone gets one, it is in every player's inventory, a late
  joiner's included; when a door or seal uses it up, it is gone for everyone. The coop save keeps
  the ledger: per key, held or used.
- **What changes the world is shared:** a door unlocked with a key opens for both.
- **Exchanges cross the line between world and player** (`map_event_interact`,
  `map_scripts.cpp`), so each is decided on its own. The leaning for each:

  | exchange | spends | gives | leaning |
  |---|---|---|---|
  | Key of the Dead | Gold Cross (world) | Key of the Dead (world) | world: once, for everyone |
  | Healing | Mirror of Truth (world) | Healing magic (player) | per player: the Mirror is not spent for the world, each player trades it once, or only one player could ever learn Healing |
  | Harp | Dragon King Grass Fruit (player) | Harp (world) | the trader's Fruit, the world's Harp; the Fruit is also the revival item, so it costs that player alone |
  | floor-3 door | nothing | a door's link (world) | world |
  | weapon transform (floor 5) | the Dragon Sword (player) | the transformed sword, a world flag | per player: each player's Dragon Sword transforms once; the doors it opens (`weapon_transformed`) are the world's, set by the first |

  The floor-3 healing tile (Resist Fire and Bless) and other learned magic are per player and
  run on the player who stands there.

## What else changes

- **Menus do not pause the world in a room.** With a partner on the floor, opening the inventory
  would freeze their enemies; in a room only the player's view stops. A change to the game's
  behaviour, but coop cannot work without it. Each menu is a blocking loop that draws its own
  frames (`menu_status_panel`'s `while (1)` around `menu_frame_begin` and
  `menu_present_frame`, and `kf::host_wait_buttons_released`, in `menu_runtime.cpp`); the save,
  load, drop, config and system panels are the same, and dialogue, shops and item messages likely
  too. Rather than rewrite each panel, the world update runs from inside the menu's frame; whether
  the game's update can be entered from there (re-entrancy, the menu's own use of the frame) is
  what the spike in Order finds out.
- **The world blocks too, not only menus.** The floor-5 boss reveal shows two images until a
  button (`screen_show_image_until_input`) and the weapon transform runs two `for (;;)` render
  loops, one moving the player along a camera path (`map_scripts.cpp`). On the owner's machine
  each would freeze the floor for the partner. Under "authority goes with the cause" they run on
  the triggering player's machine; when that player is also the floor's owner, the world update
  has to run from these loops' frames as from a menu's.
- **Saves:** the coop save is the world, the key ledger, the host's character and each guest's
  slot, each slot with its own per-player chest record, kept on every machine.
- **The pose** gains the game update number and the height (on a stair or in the air the knight
  is drawn on the cell's floor today).

## Order

1. **The table of the player's reads**, before any code: every read of `player_state`,
   `actor_state.player_position` and `camera_position` in the floor code (scripts, objects, events,
   actors, effects, collision), each classed as the triggerer's, the owner's or the world's. It is
   a day's work and decides whether "authority goes with the cause" holds; a read that fits none
   of the three changes the model before anything is built.
2. Two spikes, since either can sink the model and the knight cannot: enemies that target more
   than one player (a per-actor target in place of `actor_state.player_position`), and a world
   that does not stop while one player is held (the world update called from a menu's frame and
   from the boss reveal's and weapon transform's loops). If either fails, the model changes.
3. The smooth knight: poses numbered by update, a playback buffer, swings as events, height.
   Needed by this model whatever else changes.
4. The coop save: the state map applied, the floor snapshot split into the world's part and each
   player's, guest slots, the key ledger, the world's highest floor, the guarded snapshot writes,
   the copy on every machine.
5. Floor ownership: the snapshot on arrival, enemies drawn as puppets, triggers sent as writes,
   the handover.
6. Hits across machines (the attack event, remote players in the collision grid); menus that do
   not pause, every panel; kill credit; the exchanges.

## Questions

1. **Who runs a shared floor:** the first to arrive, or always the host? Leaning first to
   arrive, which avoids a handover every time the host walks in.
2. **Shop stock:** per player, as chests are, or shared with the first buyer taking it? Leaning
   per player: it follows chests and keeps the single-player economy, and a shared limited stock
   is an item race.
3. **Where a late joiner starts:** beside the host, at the game's start, or at the host's last
   save point? Beside the host on a deep floor, a level-1 character dies at once and goes back to
   its last save, which for a new character is the start, so beside the host is a loop. Leaning
   the game's start.
4. **The host drops mid-session:** the room ends for everyone, or a guest's copy carries on until
   the host returns? Carrying on means two copies of the world that both changed, and key spends
   with no one to arbitrate them. Leaning the room ends: each guest keeps the writes the host
   never received and they are merged at the next session.

## Weak points (to argue with)

- **Is per-player loot faithful?** The original never had to decide it, and two copies of a
  unique weapon change the game's economy: close to a modern convenience. Accepted: each player
  gets the single-player economy.
- **Which ids are key items:** 52 to 76 also holds things that are used rather than spent on a
  door (Mirror of Truth, Harp, the staves, Dragon Chalice). Each needs checking against the game:
  held by the world, or per player. The exchanges table is the same question for trades and is a
  first pass, not checked against every page of dialogue.
- **Persistent actors killed while a player was away, or before a player's death sent them back
  to their last save,** stay dead; their drops are a boss's, so that player has to get them some
  other way (given on rejoining or on going back?).
- **Triggers on a floor someone else owns:** a write sent from the triggering player reaches the
  owner a moment late, so the owner can briefly see a door shut that the partner has opened, or
  run an event the partner's write has just changed. Whether any trigger needs the owner's answer
  before it acts (a door an enemy is standing in) is for the table in Order to find.
- **Handover mid-fight:** the owner walks off the floor while enemies are chasing the other
  player; the snapshot must carry AI state well enough that they do not reset or jump. Smaller
  than feared: `KfActor` is about 70 bytes and its only pointer, `animation_cache`, can be
  rebuilt. Still to check: what the `unknown_*` fields hold, AI state kept outside the actor
  (statics in `actor_behavior.cpp`), and doors and effects caught mid-motion.
- **The floor's owner drops:** its enemies, their HP and the floor's unsaved progress go with
  it, and the partner is left with frozen puppets. The partner takes the floor over from what it
  last received (puppet poses, HP from hit events) and the floor's last versioned record.
- **Trust:** hits judged by whoever feels them let a modified client never be hit. Fine between
  friends; not if rooms are ever public.
- **A guest without the host:** the guest's character only exists in the host's save, so a
  guest whose host stops playing loses that character. The guest's backup copy holds it but is
  not playable alone.
- **Death is softer than the game's.** Without a revival fruit the original starts over; coop
  sends the dead player to their last restore point instead. Taken because a new game would wipe
  the partner's world, but it is a convenience the original did not have.

## Changes

- 2026-10-26: first draft, from the discussion that ruled out the summon model and lockstep.
- 2026-10-26: decided: the coop world is the host's save, guests' characters live in it, the host
  may play it alone; late joiners start fresh; full experience to everyone on the floor; death
  goes back to the character's last save; non-chest items per player; key items held by the
  world. The lasting world is the game's floor snapshots, not flags, so merging by union is gone.
- 2026-10-26: red-teamed against the decomp. The game does keep story flags (floors 1, 3 and 5),
  shop stock and learned magic, so the lasting world is corrected and a state map added. Enemies
  know one player, so choosing among players is added to the model; menus are blocking loops, so
  the plan for not pausing is the world update run from the menu's frame. Both are spikes at the
  head of Order. The coop save is copied to every machine with versioned writes, and key spends
  go through the host ("nothing to merge" missed a key spent on two floors at once). Death follows
  the game's revival and restart paths. Late joiners' start and shop stock are open questions.
- 2026-10-26: red-teamed again. The floor code knows one player well beyond the enemies (scripts,
  traps, collision, effects), so whoever owned a floor would run the partner's healing tile, boss
  trigger and chests against the owner: authority now goes with the cause, what a player
  triggers running on their machine and the owner running only what happens on its own, and the
  first step in Order is a table of the player's reads to test it. NPC stages came from the
  loader's highest floor and could be rewound by a new guest, so they read the world's. Trades
  cross world and player (Mirror of Truth for Healing would teach only one player), so an
  exchanges table. The boss reveal and weapon transform block like menus and join that spike.
  The game's own snapshot writes would let a non-owner overwrite the floor, so they are guarded.
  Enemy melee is decided in the AI, so an attack event; remote players enter the collision grid.
  Death without a fruit is a new game in the original, so going back to the last save is marked
  as a departure. The guest's copy is a backup, not a way to resume; the host dropping is
  Question 4.
