# Coop plan

**Draft, 2026-10-26, red-teamed three times; Order's first three steps done 2026-10-27, the fourth's
interfaces agreed.** Built so far: the two spikes and the smooth knight
([PLAYER_MODEL.md](PLAYER_MODEL.md)). When a section changes, edit it and add a line to
"Changes" at the end rather than leaving the old text standing. Transport, rooms and the service
are [NETWORKING_PLAN.md](NETWORKING_PLAN.md)'s.

The goal: **one seamless playthrough together**, start to end, for a party of any size. The
players are equals in one world, each can be anywhere in it, including another floor, and they
share one pack.

## What players are told

The test of the design: if a rule needs more than a line here, it is too complicated.

- The party shares one pack and one purse. Whatever anyone finds goes into it, and the side of
  the screen says who found what.
- Wear anything in the pack that no one else is wearing. Gear is lent, not owned: when someone
  leaves, what they wore goes back in the pack.
- The world is shared: a chest anyone opens is empty for everyone, a door anyone opens is open
  for everyone.
- A kill on your floor gives you its full experience.
- Menus do not pause the game in coop.
- Die and you keep everything: you watch another player, and come back beside them once they
  are out of a fight.
- If everyone goes down, the party goes back to its last save, the world with it.
- If the host leaves, the session ends; everything up to that moment is kept.
- Joining sets your own game aside, and leaving puts it back where you stood.
- A room is private, joined by its code, or public, listed for anyone to join; each keeps its own
  world.

## Fixed

- **Any number of players.** No rule below counts the players. How many a room holds is
  measured, not chosen: the weakest platform's cost of drawing the knights and taking their
  poses (the 3DS's, `RUNTIME.md`), and the host's upload. Four is the first number to test.
- **One tick rate per room, the host's.** The game counts updates, not time, so the rate is its
  speed; players at different rates would walk and swing at different speeds. Joining a room
  sets the guest's `gameplay.tick_rate` to the host's for the session.
- **Not a summon.** Joining the host's floor with the guest's own world set aside (Dark Souls)
  makes the guest a visitor, not a member of the playthrough.
- **The coop world is the host's save,** separate from their single-player saves. It holds the
  world, the party's pack and purse, and a character slot for every guest who has joined; a
  guest's character lives there and nowhere else (the guest's machine keeps only an export of
  it). The host may play it alone between sessions; a guest only plays it in the host's room.
- **Joining sets the player's own game aside; leaving puts it back.** The game in progress is
  kept as it stands (character, world, pack, automap, where they stand), in memory and in a file
  of its own, so quitting or a crash in the room loses nothing; it is the quick save's path, a
  third card file beside the slots' and the quick save's. Leaving, the host dropping or the
  connection failing all bring it back, the floor reloaded. A join asked for in a menu, a
  dialogue or a scripted loop happens at the next point between updates, as the F1 language
  change does; from the title it is at once, and leaving goes back to the title.
- **A coop world per room code,** kept by its host: a group keeps its world under its code, and
  another group's does not touch it. The host plays one alone between sessions from the Online
  panel, which sets their own game aside the same way.
- **The host is whoever is in the room first,** and the panel says whose world it is. If the
  wrong player arrives first, they leave and come back; handing a world to its owner is not built.
- **Public rooms.** A room is private (joined by its code, as now) or public: the service lists
  public rooms of the same build, with their host's name, floor and number of players, and anyone
  may join one. A public room's world is its own, under a code the service gives it, so strangers
  never touch a private world. In a public room the host can send a player away (their
  character stays in the world, worn gear back in the pack), and a host who sends someone away
  is not offered that room by the service again.
- **Late joiners start fresh, beside the host:** level 1, adding the game's starting kit (a
  Short Sword, a Medicinal Herb, 150 gold) to the party's pack and purse, and free to wear
  whatever in the pack no one is wearing. On a deep floor a level-1 character dies often, but
  death costs it nothing but time (Death), and the experience rule brings it up quickly. No
  hand-made catch-up.
- **Experience:** every player on the floor gets a kill's full experience.
- **Death is the party's, as in Halo's coop.** A player who dies keeps everything and watches
  a living player through their eyes (the next one at a button press); they come back on a free
  cell beside the one they are watching once that player is out of a fight (no hit taken or
  dealt for a few seconds, and no enemy chasing them), on whatever floor that is. A dead player
  is out of the world meanwhile: enemies do not target them, the collision grid does not count
  them, and kills give them nothing.
- **When everyone is down, the party goes back to the last save:** every character and the world
  as they were when anyone last saved, all standing at that save point. The game's own death
  paths (`player_death_restart`, `player_death.cpp`: the revival on floor 1, and the restart
  through `game_state_initialize()`, a new game) never run in a room. **This is a departure
  from the game**, chosen for fun: dying apart is the coop game's worst moment, and here a death
  costs time, while the game's tension moves to the wipe. A host playing the coop world alone is
  a party of one, so every death of theirs is a wipe.

## Rejected

| model | what | why not |
|---|---|---|
| summon | the guest joins the host's world while there | not a playthrough together |
| lockstep (and rollback) | every machine simulates the whole world from everyone's inputs | the game holds one floor in memory, so every player would be held to the same floor; also input delay, the slowest player sets the pace, menus pause everyone, a desync is fatal and every platform must be bit-identical |
| host-authoritative with prediction | the host simulates everyone, the guests predict and are corrected | built for not trusting clients, which coop does not need; a guest's movement would be re-run against the host's collision and corrected |

## The model: authority goes with the cause

The floor code knows one player, and not only in the enemies: floor scripts read the player's cell
and facing (`map_scripts.cpp`: the floor-3 healing tile, the floor-5 boss trigger), traps fire from
the object pool (`map_object_pool.cpp`), the collision grid counts one occupant
(`collision.cpp`, `collision_adjust_cell_occupancy`) and enemy projectiles test one camera
(`effect_update.cpp`). A machine that ran a whole floor for several players would show the others
a floor that ignores them. So authority is split by what causes a thing, not by floor:

- **Each player owns their character:** movement, level, vitals, magic and what they wear, applied
  at once on their own machine (the pack is the party's, see Items). Others see it as a puppet:
  poses numbered by game update, buffered and interpolated, with actions (swings, spells, deaths)
  sent as events and animated locally at the game's rate. Poses go only to the players on the
  same floor; the rest need only which floor a player is on.
- **What a player triggers runs on that player's machine, against that player:** cell scripts
  (the healing tile, the boss trigger), pickups and chests, talking to an NPC and its exchanges,
  switches, doors opened by hand, restore points. These already read `player_state`, which on the
  triggering machine is the right player, so they run unchanged. What they change in the world (a
  flag, a door's link, an event's stage) is sent as a write to the floor's owner.
- **What happens on its own is run by the floor's owner:** enemies, trap emitters, moving objects,
  timers. With one player on a floor, that player's machine is the owner and nothing changes.
- **A shared floor is run by its most senior player:** the host, then the guests in the order
  they joined the room. A dead player counts as on the floor of the player they are watching.
  Whoever arrives on an occupied floor needs the live floor (enemies' positions, HP and AI state,
  emitters, doors) as a snapshot anyway, so the snapshot is the handover: one who arrives more
  senior than the owner is sent it and takes the floor; any other draws those enemies as
  puppets, as it draws the other players, still running its own triggers and with the puppets
  in its own collision grid so it cannot walk through them. An owner who leaves sends a fresh
  snapshot to the next most senior player there, who takes the floor.
- **An owner who drops takes the floor's live state with it.** The next most senior player on
  the floor takes it from the floor's last record, kept by the host, as if everyone there had
  just arrived: the players stay where they are, persistent actors killed stay dead, and the
  floor's other enemies start again as on entering it. It loses a fight in progress and never
  rebuilds a floor from puppets. A dropped host ends the room.
- **The unit may be smaller than a floor.** Floor 5's gates swap the actor models
  (`player_warp_same_floor` calls `map_variant_assets_load`, `player_warp.cpp`, `resources.cpp`)
  and release the animation cache, so players on floor 5 in different variants hold different
  models for the same actors. Whether a variant is a region of its own for ownership, or its
  actors only ever meet a player in it, is for the table in Order.
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
- **The coop save keeps two states:** the live one, written continuously and kept when a session
  ends, and the last save, a copy of the whole coop world (every character and the world) taken
  when anyone saves at a save point. A wipe loads the last save; nothing else does.
- **The coop save is the host's, and only the host's.** The floor's owner is the only writer of
  that floor's world records and script flags; a triggering player's world changes reach them
  through it. Every write, and every guest's character, goes to the host, and is kept when the
  host acknowledges it. A guest is sent what it needs (its slot, the records of the floor it
  loads) and keeps nothing but an export of its character. When the host drops, the room ends
  and the guests lose only what the host had not yet acknowledged, a second or so; there is
  never a second copy to merge, even when the host plays alone between sessions. Key spends are
  not a floor's: they go through the host, the first claim wins and a door refuses the second.
- **The game's own snapshot writes are guarded.** `map_world_state_persist` runs on revival
  (`player_death.cpp`), at a save point (`map_scripts.cpp`), on F2's quick save and on leaving
  a floor (`player_update.cpp`, `map_events.cpp`). On a machine that does not own the floor, those calls
  write only its player's part; otherwise its stale copy would overwrite the owner's.
- **NPC progress comes from the world, not the loader.** On loading a floor the game sets each
  NPC's dialogue stage to the loading player's `highest_floor`, capped by the NPC's limit, and can
  lower it (`map_event_refresh_dialogue_stage`, `map_event.cpp`); `map_load.cpp` also disables the
  floor-2 departing event by it. A level-1 guest who owns floor 1 would rewind the party's NPCs
  and the write would carry the rewind into the host's save. In a room these read the world's
  highest floor (the highest any player has reached, kept in the coop save) and a stage is never
  written lower.

## The state map

Every field of the save (`SavedGameState`, `save_system.cpp`) is the world's, the party's or a
player's:

| field | whose | |
|---|---|---|
| `player` (`KfPlayerState`) | player | level, vitals, what they wear, position, current floor; `gold` is the party's |
| `player.progress_state.highest_floor` | player and world | the player's own for warps; the world keeps the highest of all players for NPC stages and the floor-2 departing event |
| `world.floor1`, `floor3`, `floor5` | world | the story flags above |
| `world.floors[n].records` | world | events, dead persistent actors, doors, object slots, drops |
| `stock`, player bank | party | the pack; what each player wears is marked in it |
| `stock`, the two shop banks | world | only the Gold Cross is limited (`shop_menu_buy`, `item.cpp`) |
| `learned` (`KfMagicLearningState`) | player | magic learned |

## The player's reads

Order's first step, done 2026-10-27 against the decomp at the pin (`build/<preset>/decomp_src`):
every read of `player_state`, `actor_state.player_position` and `camera_position` in the floor
code, directly or through `player_distance_to_point`, `player_apply_damage` and the like, classed
as the **triggerer's** (runs on the machine of the player who caused it, against that player),
the **owner's** (the floor's owner runs it, choosing among the players on the floor) or the
**world's** (the shared record). The player's own update, menus and drawing read their own player
and are left out. Every read fits one of the three; the ones that changed the plan are marked †.

| where | what it reads | class |
|---|---|---|
| `map_ambient_script_floor3` | cell on the healing row: vitals, Resist Fire and Bless | triggerer's |
| `map_ambient_script_floor5` | cell and facing: the boss reveal, `boss_encounter_started` | triggerer's; writes the world |
| `map_ambient_script_floor1` † | cell entering, then leaving, two regions: wakes an actor, breaks the cross, removes an object | triggerer's, but its stage is two steps (below) |
| `map_interaction_dispatch` | the probe ahead of the camera: talk, shops, chests, pickups, gold, doors, switches, restore and save points, signboards | triggerer's; writes the world, and the pack and purse |
| `map_action_script_floor1`, `3` † | the pack holding the Dragon Chalice (opens the passage), the Wind Blade Bracelet (teaches Wind Cutter), run after any interaction on the floor | the pack is the party's; Wind Cutter is learned by whoever interacts on floor 3 (below) |
| `map_action_script_floor2`, `5`, `map_event_interact` | a dialogue just started: the floor-2 transfer, the weapon transform, the exchanges | triggerer's; writes the world |
| `player_use_item` (keys, seal stones, the Dragon Chalice) | the probe ahead: `map_object_pool_trigger_link` | triggerer's; writes the world |
| `player_use_item` (the Harp) † | floor 2 or 3: a floor deformation, an effect that raises cells of the height grid over time | started by the triggerer, then the floor's shape (below) |
| `player_warp_trigger_update` | the warp cells, the ending with `boss_defeat` | triggerer's |
| `player_update`, `magic.cpp` (attacks, spells) † | the actor in the aiming cone, kept in `actor_state.player_target`; the hit applied by `actor_apply_damage` | triggerer's; the hit and the aim go to the owner (below) |
| `actor_apply_damage` † | the hit's credit (`KF_ACTOR_DAMAGE_CREDIT_PLAYER`): experience on a kill, magic or power training on any hit | experience to every player on the floor; training to the hitter only |
| `actor_apply_damage`, `actor_play_sound_at_phase`, the death and post-death actions | current floor is 5 or 4 (the boss, the floor-4 transform) | the floor, so the owner's |
| `actor_player_distance` (activation, active range, range exit), `actor_bearing_to_player`, pursuit | `actor_state.player_position` | owner's: the nearest player |
| `actor_try_activate_nearby` | `player_state.allow_near_actor_spawn`, set for one update after a load or warp | owner's: kept per player, so only a player who has just arrived lets an enemy spawn beside them |
| `actor_try_attack_player`, jump contact | the player's position at the attack frame, `player_apply_damage` | owner's choice of target; the hit judged by the victim (attack event) |
| actor spells (`actor_behavior.cpp`, the aim in the cone) | `player_distance_to_point_in_cone`, `player_position` | owner's: aimed at a player in the cone |
| `effect_update_homing_direction` (`KF_EFFECT_HOMING_PLAYER`), the actor-spawner effect | `camera_position` every update | owner's: the target chosen at launch and kept in the effect |
| effects' hits (`effect_dispatch.cpp`, `effect_update.cpp`), radial blasts | `collision_query_world`'s player test, `player_apply_radial_damage` | judged by the victim, as attack events |
| `effect_magic_power` | the player's magic for an effect flagged `KF_EFFECT_USE_PLAYER_MAGIC` | the caster's; on the owner, the weapon transform's and the boss death's blasts read the owner's magic (accepted) |
| `collision_query_world` | the cell's occupant count, then the one player | each machine: remote players in the grid and in this test |
| `map_object` projectile emitters | `player_distance_to_point` | owner's: any player in range |
| `map_object` reveal pieces | current floor; `revealed_piece_count`, `revival_enabled` | world |
| orbit sounds, `audio_*`, the warp shimmer's facing | the camera | each machine's own presentation |
| `map_event_refresh_dialogue_stage`, `map_load` floor 2 | `highest_floor` | world (the world's highest floor) |
| `map_world_state_persist` | current floor: revival, the **save point** (not the restore point), F2 quick save, leaving a floor | guarded: the owner's only |
| `map_variant_assets_load`, `player_warp_same_floor` † | `player_state.map_variant` | each machine's drawing (below) |

What the table changed:

- **Floor 1's two-step triggers keep their stage per player.** The cross and the removed object
  wait for one player to enter a region and then leave another. With the stage in the world, one
  player's entry and another's position outside would finish it. The stage is the triggerer's
  until it completes; only completion is a world write.
- **The floor's shape is applied on every machine on the floor.** Copy regions (passages
  opening), the Harp's floor deformation and doors change what each player collides with, so the
  owner's world writes are not only recorded but applied by every player on the floor, the Harp's
  deformation started on each from an event with its update number.
- **The aim goes with the hit.** `actor_state.player_target` lets the actor a player aims at
  choose its next action at once (`actor.cpp`); it goes to the owner with the player's hits, one
  per player.
- **Floor 5's variants are not regions of their own.** A variant swaps only the actor models
  (`CHR<n>.MIM` into the actor asset slots); the placements, definitions and actor pool are the
  floor's. Floor 5's placements (`B5/MIXA.DAT`, 55 actors) are not split by the gates: the
  activation range is 14 cells, and the entry area's actors at (39,61) and (35,64) are within it
  of the ending area's arrival at (39,47), so the game itself already runs some actors with
  another variant's models out of sight. The floor stays the unit; the owner times every actor by
  its own loaded variant, as the game does, and a machine in another variant draws a puppet with
  its own models, the clip clamped to what that model has.
- **More of the world blocks.** Besides menus, the boss reveal and the weapon transform:
  the floor-4 transform (`actor_transform_definition5_to6`) runs a render loop from the owner's
  actor update, and the floor-2 transfer fade, a chest's lid, an NPC's talk animation
  (`map_event_advance_animation_blocking`), signboards and the warp shimmer loop on the triggering
  machine. All join the second spike.

## Items

- **One pack and one purse for the party.** The player bank of `item_stock` and `gold` are the
  party's, kept by the host; every change to them goes through the host. A guest's game acts at
  once and the host settles: each update the guest compares the game's pack and purse with its
  copy and sends the difference, and the host applies what it can (never below 0, below what
  others wear, or past a full stack). No write in the game slips past and nothing waits; two
  players using the last herb within a round trip both heal, and the pack stays at 0. Stacks hold 99
  (`KF_ITEM_STACK_CAPACITY`, `item_types.h`). The world holds exactly one game's loot, as in
  single player: a chest, a drop, a boss's drop, an NPC's gift and a shop's one Gold Cross are
  taken once, into the pack. Keys are in the pack like anything else, so a key spent on a door is
  gone for everyone. Shops sell everything else without limit, so the scarcity is in the unique
  finds: every knight can buy the shop's armour, one wears the Knight Plate from the chest.
- **What a player wears is lent from the pack, not theirs.** An item worn by another player who
  is in the room shows as worn by them and cannot be put on. When a player leaves the room,
  everything they wore goes back to the pack and their slots are empty in the save, so whoever
  plays on can wear it; a returning player puts back on whatever of it is still free.
- **The side of the screen says who found what:** "PLAYER1234 PICKED UP KNIGHT SWORD", for
  pickups, purchases, sales, items used up and keys spent, out of the way of play. The game's own
  pickup message still shows to the player who picked it up.
- **The feed is drawn by the game, on every platform alike.** Not by the Dear ImGui overlay,
  which is a desktop tool (Android puts its own panel in its place, and not every platform will
  have it): the feed is drawn into the game's own picture with the game's own fonts, as the PORT
  SETTINGS page is (`menu_spell` and `menu_draw_text`, `verdite-port.md`), so it shows wherever
  the game does, at the game's resolution, in every renderer tier. What the feed holds (its lines,
  how long each stays, the fade) is a libverdite model, game-agnostic and unit-tested; KF1 only
  draws it. The font sets the limits: player names are what `menu_spell` can spell (capitals and
  digits in English, kana in Japanese), so a name is entered in that set; item names are drawn as
  the game's menus draw them (how is for Order). Any other coop text on screen, such as whose
  eyes a dead player is watching through, follows the same rule.
- **Learned magic is a character's.** The floor-3 healing tile (Resist Fire and Bless) teaches
  whoever stands on it.
- **Exchanges** (`map_event_interact`, `map_scripts.cpp`) are the single-player trades, made
  from the party's pack: the Gold Cross for the Key of the Dead, the Harp's trade, the Dragon
  Sword's transform, once each. The Mirror of Truth for Healing is the one that matters to a
  party, since Healing is learned by the knight who trades (Questions).

## What else changes

- **Menus never pause the world in a room,** wherever the others are. One rule a player cannot
  get wrong, where pausing only when alone on the floor would make the same menu safe one moment
  and deadly the next. Opening the inventory would otherwise freeze the others' enemies, so only
  the player's view stops. A change to the game's behaviour, but coop cannot work without it. A
  player in a menu still feels hits: their machine judges attack events and projectiles from the
  menu's frame, or a menu would be a place no enemy can touch. On the floor's owner the menu's
  frame runs the whole world update; on any other machine it only takes messages and judges hits
  against a player who is standing still. Each menu is a blocking loop that draws its own frames
  (`menu_status_panel`'s `while (1)` around `menu_frame_begin` and `menu_present_frame`, and
  `kf::host_wait_buttons_released`, in `menu_runtime.cpp`); the save, load, drop, config and
  system panels are the same, and dialogue, shops and item messages likely too. Rather than
  rewrite each panel, the world update runs from inside the menu's frame; whether the game's
  update can be entered from there (re-entrancy, the menu's own use of the frame) is what the
  spike in Order finds out.
- **The world blocks too, not only menus.** The floor-5 boss reveal shows two images until a
  button (`screen_show_image_until_input`) and the weapon transform runs two `for (;;)` render
  loops, one moving the player along a camera path (`map_scripts.cpp`). On the owner's machine
  each would freeze the floor for the others. Under "authority goes with the cause" they run on
  the triggering player's machine; when that player is also the floor's owner, the world update
  has to run from these loops' frames as from a menu's.
- **Saves:** the coop save is the world, the party's pack and purse, and every character, kept on
  the host, live and as last saved. A save point in a room saves the whole party, wherever each
  player is; the save panel picks no card slot.
- **The ending is everyone's.** Walking into floor 5's ending cell with `boss_defeat` set ends
  the game (`player_warp_trigger_update`, `player_warp.cpp`); in a room it plays for every
  player, wherever they are, and the coop world is marked finished.
- **The pose** carries the game update number and the height (done, Order's third step).

## Order

1. **Done 2026-10-27** (The player's reads). **The table of the player's reads**, before any code: every read of `player_state`,
   `actor_state.player_position` and `camera_position` in the floor code (scripts, objects, events,
   actors, effects, collision), each classed as the triggerer's, the owner's or the world's, and
   every read of `player_state.map_variant` (whether floor 5's variants are regions). It is
   a day's work and decides whether "authority goes with the cause" holds; a read that fits none
   of the three changes the model before anything is built.
2. **Done 2026-10-27, both hold** (The spikes). Two spikes, since either can sink the model and the knight cannot: enemies that target any of
   several players (a per-actor target in place of `actor_state.player_position`), and a world
   that does not stop while one player is held (the world update called from a menu's frame and
   from every blocking loop in the table). If either fails, the model changes.
3. **Done 2026-10-27, but for the 3DS** (The room's size). The smooth knight: poses numbered by
   update, a playback buffer, swings as events, height. Needed by this model whatever else
   changes. Then the room's size measured: knights drawn and poses taken on the weakest
   platform, the host's upload.
4. **Interfaces agreed 2026-10-27** (`verdite/pickup_feed.h`, `port/ext/coop_party.h`,
   `port/ext/coop_message.h`, `kf/platform/coop.h`). The coop save: the state map applied, the
   party's pack and purse with worn gear lent from it, guest slots, the world's highest floor,
   the guarded snapshot writes, the acknowledged writes to the host, the player's own game set
   aside and put back, a world per room code. The pickup feed: libverdite's model, drawn by the
   game; the cause of a change (found, bought, sold, used) is said by the shop's buy and sell and
   item use, since a difference in the pack cannot tell. Built in that order: the models with
   their tests, the file, the session, the game's patches, the feed. Until step 5 only the host's
   world records go into the coop save; a guest alone on another floor keeps its chests and
   doors for the session. Public rooms follow, with the service's list (NETWORKING_PLAN.md).
5. Floor ownership: seniority, the snapshot on arrival, enemies drawn as puppets, triggers sent
   as writes, the handover, the floor restarted from its record when its owner drops.
6. Hits across machines (the attack event, remote players in the collision grid); menus that do
   not pause, every panel; kill credit; the exchanges.
7. Death: watching another player (the arrival path, drawn from their camera), coming back
   beside them, the wipe loading the last save.

## The spikes

Order's second step, built 2026-10-27 and kept, since what they built is what coop needs. Both hold.

**Enemies that target any of several players.** One substitution point: `actor_pool_update`
points `actor_state.player_position` at each actor's nearest player on the floor (this one, or
another in the room from their last pose, on the floor's height at their cell) while that actor
updates, and puts it back after. Every read through `player_position` (activation, range,
bearing, pursuit, the attack's reach) then follows the actor's target; `actor_target_players`
keeps whom each goes after. A swing at another player is not felt here (`actor_try_attack_player`
returns before `player_apply_damage`); the attack event that lets the victim feel it is step 6.
Checked with two headless instances in a room: B walked to floor 1's enemies at cells (24..26,
10..11), out of A's range; on A they woke and went after B's id as on B, and A took nothing.
Alone, every target is this player and the game is unchanged. Still reading the one player:
actor spells' aim (`player_distance_to_point_in_cone`), homing effects and the actor spawner
(the table's owner's rows); each becomes the actor's target the same way.

**A world that does not stop while one player is held.** The world's update after
`player_update` is one function, `game_world_update`, and every held loop draws through
`render_frame` (its `frame_pacer_wait`) or `kf::host_wait_frame` (menus, dialogue pages,
full-screen images, button waits), so those two run it when it is due at the game's tick rate,
while a floor is in play and the player is in a room (`KF_TEST_WORLD_RUNS_WHILE_HELD=1` forces
it for tests). It never runs inside itself, so a loop in the world's own update (the floor-4
transform) holds the world as the game does. Checked headless, plain and under ASan and UBSan:
in the main menu floor 1's enemies kept moving and one beside the player hit it from 30 HP to 0;
talking to the NPC at (66,81) the enemies four cells off kept moving through the talk animation
and the dialogue page. What it found:

- **The NPC being talked to is held out of the world's update.** The talk animation
  (`map_event_advance_animation_blocking`) waits for the event's phase to reach a pose, and the
  event update advanced and wrapped the same phase, so the talk never ended.
  `map_event_in_conversation` is skipped by `map_event_pool_update` until the talk is over.
- **A death in a menu lands only when the menu closes.** HP reached 0 in the menu and the menu
  stayed open, the enemy still swinging; closing it ran the death. A menu has to close when its
  player dies (step 6, with menus that do not pause).
- **A slow update can step the world twice in one frame.** The main loop's `render_frame` steps it
  again when the updates before it took longer than the tick; to watch for on the weakest
  platform.
- Not yet driven: a chest's lid, the boss reveal, the weapon transform, the floor-4 transform, the
  floor-2 transfer and the warp shimmer; they draw through the same two waits.

## The room's size

Order's third step, 2026-10-27. Another player is now sent a pose every game update and played
back a few updates behind, so every update of their walk and swing is drawn, on the height they
stand at ([PLAYER_MODEL.md](PLAYER_MODEL.md)). A swing is not an event of its own: it advances a
fixed step an update, so any pose inside it gives every update of it, and lost poses cost nothing
the playback cannot fill in. The attack event in step 6 is for its target to feel it.

What a room costs each player, for N players:

- **Upload:** a 27-byte pose to each of the other N - 1, at the tick rate (12 to 30 a second, 15
  by default), about 112 bytes with the UDP, DTLS and SCTP headers (estimated, not captured):
  1.7 KB a second to each other player at 15, so 5 KB a second for four players, 10 KB at 30
  updates. Poses go peer to peer, so the host sends no more than anyone. Not the limit.
- **Drawing:** about 100 µs a knight on the desktop to pose and enqueue it, so three knights are
  0.3 ms of a 67 ms update there. The 3DS's ARM11 is far slower and has no SIMD; its cost is the
  number that sets the room's size, and it waits for a build that runs there. If it is too much,
  the weapon's morph and the per-part lighting are where to look first.
- **Poses taken:** decoding and playback are a map lookup and a blend per player per update,
  nothing beside the drawing.

## Questions

1. **Healing for a party.** Herbs are sold without limit and restore points heal for free, so
   what runs short is gold: each kill drops its gold once, for the whole party. Leaning: a kill's
   gold counts for each player on the floor, as its experience does, and the Mirror of Truth is
   not spent in a room, so every knight who trades it learns Healing.

## Weak points (to argue with)

- **Some knights trail others.** One copy of each unique find for the whole party means one
  wears the Knight Plate and the rest wear what the shops sell. Taken for the scarcity, which is
  King's Field's; more attackers and full experience for each make up for it.
- **Anyone can empty the pack.** Shared herbs and gold are spent by whoever gets there first.
  Fine between friends, and the side of the screen says who spent what.
- **Triggers on a floor someone else owns:** a write sent from the triggering player reaches the
  owner a moment late, so the owner can briefly see a door shut that another player has opened,
  or run an event their write has just changed. Whether any trigger needs the owner's answer
  before it acts (a door an enemy is standing in) is for the table in Order to find.
- **Handover mid-fight:** the owner walks off the floor while enemies are chasing the others; the
  snapshot must carry AI state well enough that they do not reset or jump. Smaller than feared:
  `KfActor` is about 70 bytes and its only pointer, `animation_cache`, can be rebuilt. Still to
  check: what the `unknown_*` fields hold, AI state kept outside the actor (statics in
  `actor_behavior.cpp`), and doors and effects caught mid-motion.
- **An owner dropping restarts the floor's fight.** Taken for never rebuilding a floor from
  puppets; it happens only when a guest who owns a floor with others on it loses its connection.
- **The host is the room.** A flaky host connection ends every session it drops, and the more
  players, the more it costs. Taken for the simplicity (no takeover, no merge); if it proves
  common, reconnecting into the same room within a grace period is the fix, not a second copy of
  the world.
- **Trust, now that rooms can be public:** hits judged by whoever feels them let a modified
  client never be hit, and a stranger can sell the pack's unique finds. Only that public world
  suffers, since it is its own, and the host can send them away; a modified client's own
  invulnerability is accepted, as it spoils only its own player's game. Names are capitals and
  digits, but can still offend: the public list needs the service's moderation (reports, a
  word list).
- **The wrong host:** whoever is first in the room hosts their world under that code, so a guest
  who arrives first starts an empty one. Leaving and rejoining fixes it; if it is common, the
  service can hold a room for the player who last hosted it.
- **A guest without the host:** a guest's character only exists in the host's save, so a guest
  whose host stops playing loses that character. The export is a record, not playable.
- **Dying to travel:** a player far from the others can die on purpose to come back beside one.
  It costs a wait until that player is out of a fight, and it brings the party together, which is
  the point. Accepted, as is a player far across the floor sharing a kill's experience.
- **What is "out of a fight"?** A few seconds without a hit and no enemy chasing may never come
  in a long boss fight, leaving the dead watching to the end. Judged by play; a longest wait
  after which they come back anyway is the fix if it drags.
- **A party against enemies tuned for one.** Enemies target the nearest player, so the others get
  free hits from behind, and full experience for each multiplies the rate; four knights may
  flatten the game. No scaling until it is judged by play; if it is needed, enemy health by the
  number of players on the floor is the lever.
- **Death is a departure.** In the original a death costs everything since the last save or the
  game. In coop one death costs only time, and a wipe costs what a reload costs. Chosen for fun;
  the danger now lives in everyone falling.

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
- 2026-10-26: red-teamed a third time, for players rather than the decomp. A room is two players,
  and the host owns any floor it is on, so the arrival snapshot is the handover and no floor is
  ever taken over from puppets when someone drops. The coop save is the host's alone, writes kept
  once acknowledged, so the guest's backup and its merge (two changed worlds once the host played
  alone) are gone. A boss's drop waits where it fell for each character and an NPC's gift is given
  to whoever lacks it, closing the drop lost to the away, the late and the dead. Dropped items are
  the world's, so players can hand each other things. A player in a menu still feels hits.
  Floor 5's variants swap actor models, so the ownership unit may be smaller than a floor. The
  ending plays for both. Shop stock is per player and late joiners start at the start. A list of
  what players are told heads the plan as its test of simplicity.
- 2026-10-26: menus never pause in a room, alone on a floor or not: pausing only when alone made
  the same menu safe one moment and deadly the next.
- 2026-10-26: death works as Halo's coop: a dead player keeps everything, watches their partner
  and comes back beside them once the partner is out of a fight; when both are down the party
  goes back to the last save, world included, unless a Grass Fruit revives the last to fall. The
  coop save keeps the last save beside the live state, and a save point saves the party. Dying
  apart was the worst moment coop had. Late joiners now start beside the host, since dying there
  no longer sends them back to the start.
- 2026-10-26: one pack and one purse for the party, replacing per-player loot: the world holds one
  game's loot, taken once, so per-player chest records, the split floor snapshot, boss drops
  waiting for each player, the key ledger and dropping items to hand them over are all gone, and
  so is the weak point that per-player loot was a modern convenience. Worn gear is lent from the
  pack and goes back when its wearer leaves. A feed on the side of the screen says who picked up
  what. Healing for two is an open question.
- 2026-10-26: the pickup feed is drawn by the game with its own fonts, from a libverdite model,
  not by the Dear ImGui overlay, so it is the same on every platform. The Grass Fruit is taken
  out of coop's death rules: it should do something in coop, not yet decided.
- 2026-10-26: any number of players, not two: no rule counts the players and the room's size is
  measured on the weakest platform (four is the first to test). A shared floor is run by its most
  senior player (the host, then guests in joining order); an owner who drops leaves the floor to
  the next, restarted from its last record rather than rebuilt from puppets. A dead player
  watches any living one and comes back beside them; the wipe is everyone down. Poses go only to
  players on the same floor.
- 2026-10-27: the table of the player's reads (Order's first step). Every read fits the
  triggerer's, the owner's or the world's, so the model holds, with five changes: floor 1's
  two-step triggers keep their stage per player, the floor's shape (copy regions, the Harp's
  deformation, doors) is applied on every machine on the floor, the aim goes to the owner with
  the hits, training goes to the hitter while experience goes to everyone, and floor 5's variants
  are not regions (they swap only models, and the game already runs actors across them). The
  floor-4 transform, the floor-2 transfer, chests, talk animations, signboards and the warp
  shimmer also block and join the second spike. The guarded snapshot writes are at the save point
  and F2, not the restore point.
- 2026-10-27: both spikes hold and are kept (The spikes): each actor goes after its nearest player
  through one substitution in `actor_pool_update`, and in a room the world's update runs from
  `render_frame` and `kf::host_wait_frame`, so menus, dialogue and scripted loops do not stop it.
  The NPC being talked to is held out of the world's update, and a death in a menu waits for the
  menu to close, which step 6 has to change.
- 2026-10-27: the smooth knight (Order's third step, The room's size): a pose every update,
  numbered and with the height, played back a few updates behind. A swing is placed from any pose
  inside it, so it is not an event of its own; the attack event stays in step 6. A knight costs
  about 100 µs to draw on the desktop and four players upload about 5 KB a second each; the 3DS's
  cost, which sets the room's size, waits for a build there.
- 2026-10-27: step 4's interfaces agreed. A guest's pack changes act at once and the host settles
  (in place of asking the host before taking, which needed a patch and a wait at each of about 20
  writes); two players using the last herb within a round trip both heal. Joining sets the
  player's own game aside and leaving puts it back, so nothing is lost either way; a world per
  room code; the host is whoever is first. Public rooms, listed by the service, each with a world
  of its own, and the host can send a player away; the trust weak point is restated for them.
