# Coop plan

**Draft, 2026-10-26, red-teamed three times; Order's seven steps done 2026-10-27, public rooms
and the title menu with them.** Built so far: the two spikes, the smooth knight
([PLAYER_MODEL.md](PLAYER_MODEL.md)) and the coop save (The coop save). When a section changes, edit it and add a line to
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
  world. Both are hosted and joined from CO-OP on the title screen.
- A name floats above each other player. The host chooses the room's speed, how long the fallen
  wait, and whether new players may join, and can send a player away.

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
  change does; from the title it is at once with nothing set aside, and leaving goes back to the
  title (The title menu).
- **A coop world per room code,** kept by its host: a group keeps its world under its code, and
  another group's does not touch it. The host plays one alone between sessions from the Online
  panel, which sets their own game aside the same way.
- **The host is whoever is in the room first,** and the panel says whose world it is. If the
  wrong player arrives first, they leave and come back; handing a world to its owner is not built.
- **Public rooms.** A room is private (joined by its code, as now) or public: the service lists
  public rooms of the same build, with their host's name, floor and number of players, and anyone
  may join one. A public room's world is its own, under a code of its own form (P and 7 letters,
  drawn at random by the host's game; a private code is 8 digits), and the service lists a room
  only for a host who asked, so strangers never touch a private world. In a public room the host can send a player away (their
  character stays in the world, worn gear back in the pack), and a host who sends someone away
  is not offered that room by the service again. (Built 2026-11-03 for any room, and refused by the
  host for the session only: Multiplayer settings.)
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
- **The handover is live.** The snapshot carries everything that runs on its own on the floor
  (every actor whole, the AI's state kept outside the actors, effects in flight, emitters, doors
  and objects caught mid-motion, the floor's timers), so the one who takes the floor carries on
  the same fight: no enemy resets, jumps or forgets whom it chases. Its completeness is tested,
  not judged: a floor captured mid-fight and restored into a fresh one must step update for update
  as the original does (Order 5 builds this first).
- **The host runs the session wherever it stands.** Owning a floor is running its simulation;
  it is not a say in the session. The host decides who owns every floor, on its own floor or not:
  each player tells the host which floor it is on, the host keeps the table of owners and is
  the only one that changes it, and every handover, arrival snapshot and restart is the host's
  order to the players concerned, so two players arriving at once never both take a floor. The
  host names each floor's heir with its owner, so an owner walking off sends its snapshot
  straight to the heir without waiting; no snapshot and no play goes through the host, which
  is on the path only of who owns what and of the records it keeps. The host takes only the
  records of a floor from the owner it named, so a former owner's late writes are refused. The save, the party, who is in the room, sending a player away and ending
  the room stay the host's alone. The host owns any floor it is on, since it is the most senior.
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
  owner's collision grid, so enemies do not walk through them. Players pass through each other:
  a player's own movement leaves players out, as it always did its own, since KF1's one-cell
  corridors would let two knights wall each other in.
- **Hits are decided by whoever feels them.** A player's hit on an enemy is judged on their own
  screen and sent to the floor's owner, which applies it. An enemy's melee hit is decided inside
  its AI on the attack frame (`actor_try_attack_player`, `actor.cpp`: distance, angle, a random
  status roll), so on a remote target the owner sends an attack event (actor, attack parameters,
  update number) instead, and the victim's machine runs the same test against its own position
  and the puppet's, so no one takes a hit they dodged on their screen. A projectile, an enemy's
  jump and a blast are decided by the owner against the victim's latest pose and applied as they
  are on the victim's machine: the projectile's flight exists only on the owner, so the victim has
  nothing of its own to test it against.
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
- **The game's own snapshot writes stay on its machine.** `map_world_state_persist` runs on
  revival (`player_death.cpp`), at a save point (`map_scripts.cpp`), on F2's quick save and on
  leaving a floor (`player_update.cpp`, `map_events.cpp`), and writes only that machine's copy of
  the world. A floor's record reaches the host's save only as the owner sends it, and the host
  keeps it only from the owner its table names, so a stale copy never overwrites the owner's.
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
| effects' hits (`effect_dispatch.cpp`, `effect_update.cpp`), radial blasts | `collision_query_world`'s player test, `player_apply_radial_damage` | judged by the owner against the victim's pose, felt on the victim's machine (Hits across machines); traps each machine's own |
| `effect_magic_power` | the player's magic for an effect flagged `KF_EFFECT_USE_PLAYER_MAGIC` | the caster's; on the owner, the weapon transform's and the boss death's blasts read the owner's magic (accepted) |
| `collision_query_world` | the cell's occupant count, then the one player | each machine: remote players in the grid and in this test |
| `map_object` projectile emitters | `player_distance_to_point` | owner's: any player in range |
| `map_object` reveal pieces | current floor; `revealed_piece_count`, `revival_enabled` | world |
| orbit sounds, `audio_*`, the warp shimmer's facing | the camera | each machine's own presentation |
| `map_event_refresh_dialogue_stage`, `map_load` floor 2 | `highest_floor` | world (the world's highest floor) |
| `map_world_state_persist` | current floor: revival, the **save point** (not the restore point), F2 quick save, leaving a floor | each machine's own copy; the host keeps only the owner's record |
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
4. **Done 2026-10-27, but for public rooms** (The coop save; `verdite/pickup_feed.h`,
   `port/ext/coop_party.h`, `port/ext/coop_message.h`, `kf/platform/coop.h`). The coop save: the state map applied, the
   party's pack and purse with worn gear lent from it, guest slots, the world's highest floor,
   the guarded snapshot writes, the acknowledged writes to the host, the player's own game set
   aside and put back, a world per room code. The pickup feed: libverdite's model, drawn by the
   game; the cause of a change (found, bought, sold, used) is said by the shop's buy and sell and
   item use, since a difference in the pack cannot tell. Built in that order: the models with
   their tests, the file, the session, the game's patches, the feed. Until step 5 only the host's
   world records went into the coop save. Public rooms and the title menu followed the same day (The title menu),
   with the service's list (NETWORKING_PLAN.md); sending a player away followed (Multiplayer settings).
5. **Done 2026-10-27, but for what a non-owner does not yet see** (Floor ownership;
   `port/ext/coop_floor.h`, `port/ext/coop_floor_owners.h`, `port/ext/coop_floor_play.h`,
   `port/ext/coop_floor_frames.h`). Floor ownership: first the floor's live snapshot and its round-trip test (a floor captured
   mid-fight and restored steps as the original), since the live handover rests on it; then the
   host's table of owners (seniority, every change the host's order, unit-tested), the snapshot on
   arrival, enemies drawn as puppets, triggers sent as writes, the live handover, and the floor
   restarted from its record when its owner drops.
6. **Done 2026-10-27, but for the Harp's deformation** (Hits across machines;
   `port/ext/coop_hits.h`). Hits across machines (the attack event, a hit on an actor another
   machine runs, remote players in the collision grid); what a non-owner does not yet see
   (enemies' effects and sounds); menus that do not pause, every panel; kill credit; the
   exchanges.
7. **Done 2026-10-27** (Death; `port/ext/coop_death.h`, `port/ext/coop_death_play.h`). Death:
   watching another player (the arrival path, drawn from their camera), coming back beside them,
   the wipe loading the last save.

## The spikes

Order's second step, built 2026-10-27 and kept, since what they built is what coop needs. Both hold.

**Enemies that target any of several players.** One substitution point: `actor_pool_update`
points `actor_state.player_position` at each actor's nearest player on the floor (this one, or
another in the room from their last pose, on the floor's height at their cell) while that actor
updates, and puts it back after. Every read through `player_position` (activation, range,
bearing, pursuit, the attack's reach) then follows the actor's target; `actor_target_players`
keeps whom each goes after. A swing at another player is not felt here (`actor_try_attack_player`
returns before `player_apply_damage`); the attack event that lets the victim feel it is step 6
(Hits across machines).
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
  player dies (step 6, with menus that do not pause; done, Hits across machines).
- **A slow update can step the world twice in one frame.** The main loop's `render_frame` steps it
  again when the updates before it took longer than the tick; to watch for on the weakest
  platform.
- Not yet driven: a chest's lid, the boss reveal, the weapon transform, the floor-4 transform, the
  floor-2 transfer and the warp shimmer; they draw through the same two waits.

## The coop save

Order's fourth step, built 2026-10-27 but for public rooms: in a room the game in play is the
room's coop world. `port/ext/coop.cpp` is the session; the party's rules and the bytes are
`port/ext/coop_party.h` and `coop_message.h` (unit-tested), and the feed's model is libverdite's.

- **Who hosts:** the client says on JOINED whether anyone was in the room first. The first in
  hosts the world kept under the room code (`coop-<room>.kfcoop` beside the saves, with a `.bak`
  of the one before), or starts a new one: a new game at the start, its pack the host's kit. A
  guest says hello to the room's first peer, the host (the service's welcome lists the peers
  oldest first), and takes a welcome from no one else; the host answers with a welcome: the
  guest's character (empty for one new to the world, who starts a new game's), the pack and
  purse, what they wear, the world's highest floor, the world's bytes and the host's cell.
- **Setting the game aside:** joining writes the game in play to a third card file
  (`BISLPS-00017KF.aside`) through the quick save's path, then puts the coop world in between
  updates as a quick load does (`player_replace_game`, the quick load's swap, now shared).
  Leaving, the host leaving, or the connection failing puts it back and removes the file. A run
  that quit or crashed in a room leaves the file, and the next run puts that game back before
  it enters any room ("Your game from before the coop session is back").
- **The pieces:** a character is the player's fields and the magic they learned, a world the
  floors, story flags and the two shops' banks, both in the earlier builds' field stream
  (`save_system_coop_character`, `save_system_coop_world`) and validated whole before any of
  it is applied, with the party's pack and purse (`save_system_coop_apply`). Worn gear is
  checked per slot against the equipment menu's ranges, since an item in the wrong slot would
  index the game's armour table out of range.
- **The pack:** both the host and a guest look at the game's pack every update, and take its
  difference from what they last put in (`CoopPackMirror`). The host applies its own at once;
  a guest sends its own, numbered. What the host decided goes into the game only between
  updates, never while a menu or script holds it, since the drop and sell lists keep indexes
  into the pack across their frames and a count changed under them could go below 0. So a
  player in a menu sees the others' changes when it closes. The shop says bought and sold
  (`coop_pack_cause`); any other change is found (more) or used (fewer).
- **Worn gear:** a wear request is numbered and the host's pack message says the last it has, so
  a guest takes off only what the host refused after seeing its latest request. The equipment,
  drop and sell lists leave out what the others in the room wear.
- **The file:** the host stores its own character every second and the guests' as they send them,
  and writes the file at most every 5 seconds, at a save point (the whole live state is also
  kept as the last save), and when it leaves the room, quitting included.
- **Guards:** in a coop world F2 and F4 do nothing and a slot's game is never loaded. A death goes
  down (Death) in place of the game's revival or restart, which is a new game and would empty the
  party's pack. NPC stages and the floor-2 departure read the world's highest floor, and a stage
  never goes lower.
- **The feed** is drawn by the game after its HUD (`menu_draw_pickup_feed`), at the left of the
  picture: the line spelled by `menu_spell` with the item's name as the menus draw it, faded
  with the shade `menu_draw_text` takes. In English "PLAYER1234 USED MEDICINAL HERB"; in Japanese
  the name, the item, the count and a verb in kana (てにいれました, かいました, うりました,
  つかいました). A player's own changes are left out of its feed: its game already says so, or it
  made them in a menu. Not yet judged by eye.

Checked 2026-10-27 with two headless instances in a room on a local service
(`scripts/coop_pair.py`, with `give` and `coop` on the agent channel), 19 checks, plain and under
ASan and UBSan: the host's new world holds one kit; a guest arrives beside the host with its own
kit added (two swords, two herbs, 300 gold) and sees the party's pack; each sees the other's sword
as worn; a guest's herb and gold spent reach the host and the host's feed says so, the guest's own not; the host's
herbs reach the guest; both using the last six herbs within a round trip leaves 0 on both; when the
host quits the guest's own game comes back; a guest that quits in the room gets its own game (with
an item only it held) back in its next run; and the host's world, restarted, keeps the pack as last
changed. Under UBSan the only reports are libjuice's (`picohash.h` calls its hash through a
mismatched function type, on its own thread, once two peers connect), so two sanitized instances in
a room need `halt_on_error=0`.

Not yet:

- **Names:** a player is `PLAYER` and four digits of the install's id (`coop.id` in the user
  folder); choosing one, and kana names for Japanese, whose font has no capitals, is to come.
- **A held host** answers a hello and puts the others' changes into its game when its menu
  closes; a guest arriving while the host reads a sign waits for it.
- The coop world's automap is not kept: each entry starts it unexplored.
- A guest's world outside the floors it runs is the welcome's: a floor it loads is brought up to
  date by the host's record or the owner's snapshot when it arrives (Floor ownership).

## The title menu

Built 2026-10-27, after Verdite2's and Verdite3's title: the game's own menu under the logo.
KF1 had none (any button at the logo started a new game, and loading was the in-game menu's), so
the logo (the opening's scene 1) now holds it, with the mouse and touch through the menu pointer. Its
top page is a line of words across the bottom of the picture, without boxes, stepped with Left and
Right (2026-11-02); the pages under it use the menu's boxes, cursor and font as the SETTINGS page does:

- **CONTINUE** (first, and where the cursor starts, when a card slot is used): the three slots
  with their floor and experience; the chosen one goes in at the game's first update. A save made with other mods asks first.
- **NEW GAME**: the story scroll, then a new game, as a button at the logo was.
- **MODS**: every installed mod with its switch (ON, OFF, ERROR or HELD), Confirm flipping it, or opening its
  page when it has a menu of its own (docs/MODDING.md, "A mod's menu"); `on_settings` and a mod's error text
  stay in the F1 Mods window.
- **COOP**, after a page saying mods stay off in there when one is on: HOST (a new private world, whose 8-digit code is shown to read out before START; a new
  public one; or a world this machine already keeps), JOIN BY CODE (a grid of digits, or the
  keyboard's), PUBLIC ROOMS (the service's list, kept up to date while shown: the host's name,
  floor and players; a full room is dimmed) and PROFILE ("The profile").

Any button in the attract scene goes to the menu, which waits as long as the logo did before
going back to the attract scene, on its top page only. F4's quick load still goes straight in.

**Hosting and joining are strict from the menu**, unlike the F1 panel, where whoever is first
hosts: a join that finds the room empty leaves and says no one is hosting (a mistyped code makes
no world), and hosting a room someone is in says it is in use. **From the title nothing is set
aside**: the coop world goes in over the new game the title starts, and when the session ends
(the player leaves, the host leaves, the connection fails) the game goes back to the title menu
(`GameResult::ReturnToTitle`, `Cutscene::Title`), not to the intro. A game a crashed run left set
aside stays in its file when the title starts CONTINUE or COOP, since either would replace it at
once; a NEW GAME still gets it back.

The menu is libverdite's (`verdite/title_menu.h`, unit-tested), so the other ports can take it;
`port/ext/title.cpp` gives it the slots, the kept worlds and the list, and joins the room;
`work/src/game/menu_title.cpp` draws it. The menu font goes up beside the logo: only
`COM/MIX.TIM`'s 4-bit pages (x 704 to 1024, y 256 to 512) and their palettes (rows 497 to 511),
which the opening's pictures never use; its 8-bit pages overlap the opening's.

Checked 2026-10-27 by `scripts/title_pair.py` with headless instances on a local service, 18
checks: the top page, a private world hosted from it and joined by its typed code, the guest back at
the title menu when the host quits, a code no one hosts refused, the kept world offered again, a
public world on the list with its host, floor and players and joined from it, NEW GAME, and
CONTINUE loading slot 1. Judged by eye: not yet (the layout over the logo, the dimmed logo
behind the inner pages).

Not yet:

- **A public room's code** is never shown.
- **Sounds**: the menu's cursor and confirm sounds are the game's bank, which the opening has not
  loaded; the title menu is silent.
- The service's check of names.

## The profile

Built 2026-10-27. What the others in a room see of a player: a name, a colour and a helmet,
chosen under COOP, PROFILE, on the title menu:

- **NAME**: a grid of the capitals and digits (12 to a line, three lines), or the keyboard's, with
  DELETE and DONE; 1 to 12 of them, which the menu font draws. Back leaves the name as it was.
- **COLOUR**: one of eight, Left and Right (or Confirm) stepping it: blue (the knight's first),
  red, green, gold, white, black, purple, orange. It paints the tabard, sleeves, knee bands and
  crest.
- **HELMET**: one of the helmets; there is one for now, the great helm, so the row is dimmed.

A change is kept at once in `coop.profile` in the user folder, beside the install's id
(`name=`, `colour=`, `helmet=` lines; a part missing or bad is the default's: `PLAYER` and four
digits of the id, blue, the great helm). It goes to the others in the next room: in the guest's
hello, and in the host's roster to everyone, so the feed, the watching line and the public list
name players by it and each knight is drawn in its player's colour and helmet. A profile is not
changed inside a room.

The rules are `port/ext/coop_profile.h` (unit-tested) and the menu's pages libverdite's.
`scripts/coop_pair.py` checks that two players each play as their profile and see the other's;
`scripts/title_pair.py` types a name, steps the colour, and sees it kept, played as and listed.
Not judged by eye: the name grid's narrower cells, the colours on the knight. In Japanese a name is
drawn with the English font's capitals, so coop needs the English files (`verdite-port.md`,
"Language handling"); `coop_pair.py --japanese a|b` plays either player in Japanese.

## Multiplayer settings

Built 2026-11-03. MULTIPLAYER, a row of the pause menu under PORT SETTINGS, there only in a coop world
and not part of PORT SETTINGS (the user's call); a Multiplayer page in F1 and Android's panel (the
option table); and on the title, COOP, SETTINGS. Settings.json's `online` keys.

| who | row | what |
|---|---|---|
| everyone | NAME BARS | ALWAYS (through walls, as Minecraft), IN SIGHT, OFF |
| everyone | PARTY FEED | the pickup feed |
| host | TICK RATE | the room's one speed; a guest's own row is dimmed and comes back when it leaves |
| host | NEW PLAYERS | CLOSED turns away anyone without a character in this world; a returning player still gets in |
| host | REVIVE WAIT | 10, 30 or 60 seconds: the most a down player waits beside a fight (it was 30) |
| host | guests' rows | SEND AWAY for each guest in the room |

- **Messages on the ground** are not a room's, so their row stays in F1 alone, as before.
- **What only the host sees:** YOUR ROOM, MULTIPLAYER's second page, only while `coop_hosting()`; in
  F1, a section shown only then. On the title, before any room exists, HOST, ROOM SETTINGS has the first three.
- **The rules travel:** `CoopRules` (tick rate, revive wait) from the host, after its welcome and
  whenever they change, and nothing else about them is trusted: only the host's are heard, and a
  value outside the choices is no message. This builds "One tick rate per room", which the plan fixed
  but the code had not done. Playback delay is worked out when a peer connects and not after a change
  of tick rate.
- **Being turned away:** `CoopTurnedAway` says why; the guest leaves as if the host had left, and the
  title says so. A guest sent away leaves the party at once on the host, as a drop does (its gear back
  in the pack, its floor to the next player), and its id is refused until the host's session ends.
  A modified client can ignore the message: the host stops hearing it, but the other players still
  see its poses until it goes, since only the service could close its connection. The refusal is not
  kept in the world's file and is not the service's yet.
- **A guest's rows** keep their slot while the guest stays, so a row never comes to mean someone else
  under the cursor. Three slots, as the service's room holds 4.
- **The name bar** (`kf/platform/online_name_bar.h`, `render_online_player_name`): the name in the menu
  font on a dark band (black at the average blend), 2030 above the feet, 14 units to a font pixel,
  so it shrinks with distance. Its corners are in the camera's axes, and it is a smoothed world sprite
  like a spell's, not a HUD quad, or it would swim against the knight while turning. It is unlit and
  unfogged. ALWAYS sorts it in front of the world; IN SIGHT at its own depth, so nearer walls paint
  over it in the game's order (the optional depth buffer also hides ALWAYS behind walls). It shows
  only where the knight is drawn, so never past the cell window, and not within 700 units of the
  camera, where it would fill the picture.
- **Checked** (`scripts/coop_settings.py`, 21 checks, three headless instances on a local service;
  `scripts/title_pair.py` for the title's pages): rows only the host is shown, a guest following tick
  rate and revive wait, name bars drawn, off and on, the feed off and on, a closed room, a returning
  player, a guest sent away and refused again. `nameBarsDrawn` counts the bars submitted; how a bar
  looks is not judged by eye: its size at range, whether it reads at 320x240, and the band's dark.

## Floor ownership

Order's fifth step, done 2026-10-27: the two foundations, then the session's part on them.

- **The snapshot** (`port/ext/coop_floor.h`): between updates, on the floor's owner, every actor
  slot whole, the actor definitions (floor 4's transform rewrites one), the effect records, the
  NPCs, the dialogue gate and the ambient scripts' countdown, the object pool (objects, doors,
  drops and their sequences), and the collision grid's occupant counts without the owner's own
  player, whom the taker counts itself. About 37 KB, under a CONTROL message's 64 KB. Animation
  caches are not in it: they are drawing's, rebuilt when a slot is next drawn. The floor's
  records, the grids' shape (copy regions, heights) and the player are not either: they are the
  world's or the player's. A restore is refused, with nothing changed, for another floor, another
  build's layout (the structs are copied whole), or a slot whose definition, cell or tile is out
  of range, or an object whose link the game's own restore would refuse.
  Since 2026-10-31 (the online security review) the sender's bytes are trusted for less: what the
  disc gave the taker for the same floor, which play never changes, is the taker's own (both
  definition tables, each actor's placement drop, each event's character, model, behaviour,
  home and radius; a shop's character is its stock bank), and an event slot free on one machine
  and not the other refuses the snapshot. An actor's position must be on the map (its cell is
  worked out from it), an event's dialogue stage as a saved event's, an effect's render ids ones
  this machine can draw, and a floor deformation's segments inside the table that
  `effect_floor_deform_line` walks (a forged one wrote past the height grid). Every actor slot's
  definition is checked, free or not (`actor_bind_current` points at it for each). Every object
  slot runs its action each update whatever its id, so every slot's link is checked against its
  action (a door's link names its partner's slot, which its swing writes), and a placed object's
  also against its definition's. An emptied slot may keep an action (a taken pickup, floor 5's
  copy regions) but not a door's, which reads its definition, and its cell must be on the map; an
  effect that moves off the map finds its floor at height 0 (`effect_floor_y`; the PS1 read past
  the grid), an actor woken at a home off the map wakes where it stands, a spawner carried off
  the map spawns nothing, a door on the map's edge marks only its cells on the grid, and an
  angle's ratio wraps as on the PS1 (dividing the lowest value by -1 faulted). A free actor slot may not be active (it is drawn and
  hit, though not updated), and an active actor's or event's clip, here and in a frame or a
  write, is fitted to this machine's model (the animation cache ends the game on a clip its model
  has not; "none" was let through before). A door the clearance probe cannot place, which ended
  the game, is probed at its own point. A spawned actor's placement tile is the invalid cell,
  which was refused before.
- **The round trip** (`scripts/floor_handover.py`): two seeded headless runs play the same
  (`KF_FUZZ`, no macros, godmode); in the second, at update 1500, the floor is captured, unloaded
  (which writes its record), loaded from that record and the snapshot put back, keeping that
  machine's player and random stream, as a taker's are its own. A floor trace (`KF_FLOOR_TRACE`:
  the fuzzer's regions, with the animation caches and the world's records left out, and the five
  grids and copy regions added) must then match update for update. `KF_FLOOR_DUMP=<update>` writes
  each region's bytes to find the field where two runs part. Checked 2026-10-27: 15 of 15 equal,
  seeds 31 to 33 on every floor, each with up to 12 awake actors and 18 effects in flight at the
  handover, for about 2000 updates after it. The first version only failed through the check
  itself: the reload reset the player's `equipment_effect_ticks`, which is the player's, not the
  floor's.
- **The host's table** (`port/ext/coop_floor_owners.h`, 10 unit tests): seniority by first
  arrival, the host first; a row (owner and heir) sent when it changes; a take for a more senior
  arrival, a show for any other, a run from the record for an empty floor or one whose live state
  was lost. An owner walking off names whom it sent its floor to, so a snapshot sent to an heir
  who had just left is caught and the next runs the floor from the record; a take's giver who
  leaves or drops before the taker says `taken` loses the floor the same way.

- **The messages** (`port/ext/coop_message.h`): the host's roster (each player's id and peer, the
  host first), so a snapshot goes straight from one player to another; a player's floor and whom
  it left a floor with, to everyone; the host's orders, to everyone; a floor's record, from its
  owner to the host and from the host to a player it orders to run a floor from the record; a
  snapshot (the floor's record with its live state, for a take, a handover or a show); `taken`;
  and a player's writes.
- **Each machine's part** (`port/ext/coop_floor_play.h`): a machine arriving on a floor tells
  everyone, and its actors stand still (`coop_runs_floor`, which gates `actor_pool_update`) until
  the host's order: a run from the record, a take or a show. The owner of a floor runs it; any other
  machine draws its actors. A snapshot is put in by loading the floor again from the record in it,
  only if that record is not what the machine loaded, then putting the live floor over it
  (`coop_floor_reload`), keeping the machine's player and random stream as the round trip does.
  A run from the record loads again whenever actors on the floor were drawn from another's. An
  owner walking off sends its record to the host and its snapshot to the heir from
  `map_unload_floor`, before the floor goes (`coop_floor_unloading`); the heir holds it until the
  host's row names it the owner. A snapshot is put in only when the host's orders allow its
  sender: a take or a show the host ordered that player to send, or a handover from the floor's
  last owner once the row names this machine. Until then it is held, one per sender, since the
  order and the snapshot come by different players. A guest owner sends its record to the host at most once a second
  while the floor changes. The host keeps a record only from the owner its table names, and the
  welcome's world carries what it kept.
- **A non-owner's floor:** the owner sends its awake actors every update on the STATE channel (28
  bytes each since step 7, which added the action, in parts of at most 1100 bytes, `port/ext/coop_floor_frames.h`, 5 unit tests), and
  the others play them back three updates behind, as the knights are: slot, definition, clip and
  phase, health, cell, position and rotation, counted in the collision grid where they stand, the
  clip clamped to what this machine's model has (floor 5's variants). A hit on an actor another
  machine runs goes to that machine (Hits across machines), since a local kill would drop its loot
  twice.
- **Triggers as writes** (`coop_floor_writes`): between updates, every machine on a floor compares
  the floor's lasting state with how it last sent or took it, and sends what changed to everyone;
  a machine on that floor puts it in. A placed object's id or link changed, or its action began
  (a pickup taken, a chest emptied, a key's link cleared, a door opened, a chest's lid); a drop
  that appeared or went; an event's state or dialogue stage, never lowered; the floor's story
  flags, raised only, with what loading the floor applies for them (floor 1's passage and the
  actor its two-step trigger wakes, floor 5's boss encounter). Floor 1's two-step triggers go only
  once complete, and floor 3's map pieces not at all: each machine counts the pieces its own
  objects reveal. The motion a write starts is each machine's own: every machine runs its objects.
  Records and writes from the network are checked before the game reads them, with the save's own
  check of a floor's record (`save_system_floor_record_valid`).
- **Checked** (`scripts/coop_floors.py`, 18 checks, three headless instances on a local service,
  plain and under ASan and UBSan, where libjuice's `picohash.h` is the only report): a guest
  arriving on the host's floor draws it, with the host's awake actors where the host has them; an
  object either takes is gone on the other; a floor a guest ran alone, and what it took there, is
  in the host's world when the host goes there; the host walking off hands floor 1 live to the
  guest, which carries on with the same actors where they were; the host coming back takes it; a
  third player draws the floor a guest runs, and runs it from the record when that guest drops.

Not yet:

- **What a non-owner does not see:** the Harp's floor deformation is not sent. Traps fire on each
  machine at its own player, and NPCs wander on each machine apart (their state and dialogue
  stage are shared). Enemies' effects and sounds are shown since step 6 (Hits across machines).
- **A player arriving takes the snapshot of the moment the owner sent it:** what that player
  changed in the round trip before (a pickup) is undone on its machine, though the owner has it.
- Not checked by the round trip: the Harp's floor deformation caught mid-sweep (its segments are a
  file static in `effect_update.cpp`), a boss fight's scripted loops, and a floor held by a menu or
  dialogue. Not driven in a room: a boss fight, the floor-4 transform, floor 5's variants.

## Hits across machines

Order's sixth step, done 2026-10-27 (`port/ext/coop_hits.h`, the messages in `coop_message.h`).
Every hit is applied in the world update, held or not, so a player in a menu feels it.

- **A player's hit on an actor another machine runs:** `actor_apply_damage`, once the hit has
  trained the hitter (training is the hitter's), sends it to the floor's owner (`CoopActorHit`:
  the slot and the definition seen there, the game's arguments, whether it was the player's own).
  The owner applies it without training itself, unless the slot now holds another definition.
  A spell's aim (`player_target`) goes the same way (`CoopActorAim`); the game keeps one, so the
  last player's aim wins.
- **Kill credit:** a kill by any player's hit gives the owner its experience and tells everyone
  (`CoopKill`); every player on that floor adds it.
- **An enemy's swing at another player** (`CoopSwing`): the owner's AI chose it, rolled the status
  in the game's order and sends the swing's reach and angle; the victim runs the same test
  (`actor_swing_reaches_player`) against where it stands and the actor as it draws it.
- **Projectiles, jumps and blasts** (`CoopPlayerHit`): the other players on the floor are in the
  collision grid while the world updates, and `collision_query_world` finds one
  (`KfCollisionKind::OtherPlayer`) unless the query leaves players out, which a player's own
  movement does. An enemy's projectile or jump that meets one, and a blast on the floor this
  machine runs, is sent with `player_apply_damage`'s arguments and applied as it is. Each
  machine's traps fire at its own player. An actor's spell aims at its target, and a homing
  projectile follows the player its actor went after (`effect_target_players`), carried through
  a handover in the floor's snapshot.
- **What a watcher sees and hears:** each effect slot says what spawned it (`effect_sources`: an
  actor, or what an actor's effect spawned), and the owner's frame carries those effects after its
  actors, 30 bytes each. The watcher draws them after its own, keeping each slot's animation cache
  and refusing a billboard or clip it lacks. A puppet's attack, hit and death sounds play as its
  clip passes the action's phase; an effect's launch sound as it appears and its impact as it
  goes, where the game plays them from about ten places.
- **Menus:** every blocking loop waits through `render_frame` or `host_wait_frame`, so the world
  runs under all of them. A player at 0 HP in a menu presses Back on every other read, so each
  panel backs out as the player's own presses would, and the death goes on.
- **The exchanges** come from the party's pack as any pack change does, and Healing goes to whoever
  trades. An event write also carries the dialogue's page and the stage's last page when the last
  page changes, so a trade ends its dialogue on every machine on the floor and is made once.
- **Checked** (`scripts/coop_hits.py`, 9 checks, two headless instances on a local service): the
  host's enemies hurt the guest beside them on the guest's machine; the guest, hurt to 0 in its
  menu, is back in play; the guest's hit lands on the host's actor and its kill gives both
  players the experience, and so does the host's own kill; on floor 2 the guest draws the host's
  fire balls and feels one.

Not yet:

- Not driven: an exchange in a room, a blast, a dodge, the sounds (not judged by ear) and the
  drawn effects (not judged by eye).
- **Spawning beside a player:** `player_state.allow_near_actor_spawn` is still the owner's alone,
  so an enemy spawns beside a guest who has just arrived only when the owner has too.
- The actor spawner's new actors face the owner's player.

## Death

Order's seventh step, done 2026-10-27: the rules in `port/ext/coop_death.h` (unit-tested), this
machine's part in `port/ext/coop_death_play.h`, the wipe in the session (`port/ext/coop.cpp`).

- **Going down:** when the death fade ends, `player_death_update` asks `kf::coop_player_died`, which
  in a coop world puts the player down in place of the game's revival or new game. Down, the
  player is out of the world: `player_distance_to_point` finds it nowhere (so no collision, trap,
  blast or spell meets it), `player_apply_damage` does nothing, the actors leave it out of their
  targets, the collision grid does not count it, the floor scripts and warps do not fire where its
  view is, and a kill gives it nothing. HP stays 0, so menus back out by themselves, and `move`,
  `warp` and `hurt` on the agent are refused.
- **The pose says so:** a pose carries two flags (since version 5), down and in a fight. A down
  player is left out of `online_players_on_floor`, so no machine draws, targets, counts or hits it;
  the pose is the one source, as continuous as the position.
- **Seeing through another's eyes:** each update, in place of its own, a down player looks from the
  eyes of an up player: the one it had, else the nearest on its floor, else the first in the room's
  order; Attack or Confirm moves to the next. Its camera, cell and feet are theirs (so drawing,
  sound and the automap follow), and the knight it looks out of is not drawn on its machine. When
  they are on another floor it goes there as a warp does, through `map_unload_floor`, so an owner
  hands its floor on and the arrival path follows: it counts as on that floor, and may run it. The
  game draws "WATCHING PLAYER1234" low in the middle of the picture, in the menu font.
- **Out of a fight:** judged by the player's own machine for its pose: a hit taken
  (`player_apply_damage`) or dealt (`actor_apply_damage` with its credit) in the last 4 seconds, or
  an awake enemy pursuing or attacking it. The owner knows its actors' targets; a machine drawing
  the owner's actors takes the nearest player up, as the owner chooses, so the owner's frame now
  carries each actor's action. An enemy near but wandering is not a fight.
- **Coming back:** at least 3 seconds down, so whose eyes is said, and then once those eyes are out
  of a fight, or after 30 seconds whatever the fight. The player stands on the first cell beside
  them a player could stand on now (the game's own collision query, about as high as their feet, no
  warp or pit), else the four corners, else their own cell, facing as they face, revived as with a
  Grass Fruit and faded in as the revival is. Arriving on a floor while down does not let enemies
  spawn beside it.
- **The wipe:** when every player in the room is down, the host sends the party back to its last
  save. The live world becomes the last save; each character is as saved, but one who joined since
  keeps what it has; everyone in the room wears what of their gear is free; and each player's game
  is put in again at the save point, with nothing set aside (`CoopWipe`, a welcome's fields and the
  wipe's number). A host alone is a party of one. A world never saved goes back to how it was
  opened, so there is always a last save.
- **What was said before the wipe:** a guest's messages still on their way when the wipe comes are
  of a world that no longer is (a write to a chest, a record, a hit, a pack change). Each guest,
  once back at the save, says so to everyone (`CoopWiped`, after everything it said before on that
  channel), and until then each machine drops what that guest sends; the host's messages need no
  such gate, since its wipe comes after all of them. A guest not yet back counts as up, so the host
  never wipes twice on stale poses.
- **The save point:** where the party stands after a wipe is kept with the last save (the coop
  file's version 2; a version-1 file is read, and stands the party where its world is opened). A
  guest's save request now says where it saved.
- **A character kept dying or down** (a state is taken at any moment, mid-fade too) comes into the
  world whole and standing.
- **Checked** (`scripts/coop_death.py`, 11 checks, two headless instances on a local service, plain
  and under ASan and UBSan, where libjuice's `picohash.h` is the only report): B dead on floor 2 goes
  down seeing through A's eyes, follows A onto floor 1 and comes back beside A whole; down, B is
  drawn by no one and no enemy goes after it; B stays down while A is chased and comes back once A
  is clear; both down, both stand at the save point whole with an object A took since back on both,
  and B draws A's floor again; A alone, dying, is back at the save point.

Not yet:

- **Judged by eye and by play:** the watching line, the cut from the dead player's fade to
  another's eyes and from their eyes to the revival's fade, and the lengths (3, 4 and 30 seconds).
- **A race at the wipe:** a player coming back as the last other goes down can be wiped with the
  rest, since the host judges by poses a few updates old.
- A down player's floor-5 variant is the entry's, whatever the eyes' variant.
- The Grass Fruit still does nothing in coop.

## Other players' spells and sounds

Done 2026-10-27, after Order 7: the user found that no one saw another player's magic or heard
their knight. Both travel in the pose (version 6), so they play in step with the knight drawn, and
neither depends on who runs the floor.

- **Spells:** what the player's update spawns, and what that spawns, is the player's
  (`KfEffectSource::Player`); the caster's machine runs its flight and judges its hits, as before
  (Hits across machines). Each pose carries those effects in flight, 30 bytes each as the owner's
  frame writes an effect, as many as fit in a packet (35). The others draw them from one set of
  slots for each other player (`effect_player_shown_records`, three), keeping each slot's
  animation cache, with the launch and impact sounds as an effect comes and goes, as the owner's
  enemies' effects are. A floor's snapshot carries the sender's spells as anyone's, not the
  receiver's player's.
- **Sounds:** the knight's swing, death, key, gravestone, Harp and warp shimmer, where the game
  plays them for its own player (`kf::online_sound_made`, `port/ext/online_sounds.h`). The pose
  carries a count of each, so a lost pose loses no sound; the others play one whose count moved,
  from where they draw the knight's head, as an enemy's is (`audio_play_spatial_default_range`).
  The level-up stays this player's own.
- **Checked:** `scripts/coop_hits.py` (each player's swing heard and Fire Ball drawn on the other
  machine, either way round), `scripts/coop_death.py` (a player's death heard on its floor); pose
  unit tests for the counts, the spells and the packet's size.

Not yet:

- **Judged by ear and by eye:** the sounds' volume and distance, and the spells drawn.
- Another player's warp shimmer is heard; its cylinder is mostly spawned outside the player's
  update, so it is not sent, and is not checked. The Harp's floor deformation is still not sent
  (Floor ownership).

## The room's size

Order's third step, 2026-10-27. Another player is now sent a pose every game update and played
back a few updates behind, so every update of their walk and swing is drawn, on the height they
stand at ([PLAYER_MODEL.md](PLAYER_MODEL.md)). A swing is not an event of its own: it advances a
fixed step an update, so any pose inside it gives every update of it, and lost poses cost nothing
the playback cannot fill in. The attack event (Hits across machines) is for its target to feel it.

What a room costs each player, for N players:

- **Upload:** a 34-byte pose to each of the other N - 1, at the tick rate (12 to 30 a second, 15
  by default), about 119 bytes with the UDP, DTLS and SCTP headers (estimated, not captured):
  1.8 KB a second to each other player at 15, so 5.4 KB a second for four players, 11 KB at 30
  updates, and 30 bytes more a pose for each of the player's spells in flight. Poses go peer to
  peer, so the host sends no more than anyone. Not the limit.
- **Drawing:** about 100 µs a knight on the desktop to pose and enqueue it, so three knights are
  0.3 ms of a 67 ms update there. The 3DS's ARM11 is far slower and has no SIMD; its cost is the
  number that sets the room's size, and it waits for a build that runs there. If it is too much,
  the weapon's morph and the per-part lighting are where to look first.
- **Poses taken:** decoding and playback are a map lookup and a blend per player per update,
  nothing beside the drawing.
- **A floor's actors:** its owner also sends every other player its awake actors each update, 28
  bytes each, and its actors' effects in flight, 30 bytes each: about 450 bytes with the headers for a dozen awake, so about 7 KB a second to each
  other player at 15 updates, sent to everyone and kept only on the owner's floor. A handover or
  an arrival's snapshot is about 40 KB, once.

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
  `actor_behavior.cpp`, the spike's `actor_target_players`), and doors and effects caught
  mid-motion. The round-trip test finds what is missed, by actor and field, rather than play.
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
  in a long boss fight, leaving the dead watching to the end. It came at once in testing: floor 1
  has an enemy that pursues from behind a wall it cannot pass, forever. So a down player comes
  back after 30 seconds whatever the fight (Death); the length is for play to judge.
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
- 2026-10-27: step 4 built (The coop save), but for public rooms. Two changes to the agreed
  messages: a hello carries no character, since the game can only make a new one by starting a
  new game, which is safe only once its own game is set aside, so a new character's state is
  empty and its machine starts one; and wear requests are numbered, with the pack message saying
  the last the host has and the world's highest floor. A run that quits in a room gets its own
  game back in the next one.
- 2026-10-27: public rooms and the title menu (The title menu). The client, not the service, draws
  a public room's code (P and 7 letters), since a random code of its own form is enough and the
  service then has nothing to hand out; a private code is 8 digits, the only characters every
  language's menu font has. Hosting and joining from the menu are strict, and a session started
  there sets nothing aside and ends at the title.
- 2026-10-27: the handover is live (a restart from the record was weighed for its cost and
  rejected as a seam), with a round-trip test of the floor's snapshot built first in Order 5. The
  host runs the session wherever it stands: it names every floor's owner and orders every
  handover, arrival snapshot and restart, so owning a floor is running it, not a say in the
  session, and two players arriving at once never both take it.
- 2026-10-27: a floor's heir, named by the host with its owner: the handover's snapshot had gone
  through the host, a hop more while the floor's enemies wait, for nothing the host decides then.
  Every snapshot now goes straight between the two players; the host only orders.
- 2026-10-27: Order 5's foundations (Floor ownership): the floor's snapshot, its round trip, 15 of
  15 equal on every floor, and the host's table. The table gained `taken` and an owner's move
  says whom it left its floor with, since a take or a walk-off crossing another player's move would
  otherwise leave the new owner waiting for a snapshot that never comes.
- 2026-10-27: Order 5 done (Floor ownership). Writes are found by comparing the floor's lasting
  state between updates, not by hooking each trigger, and go to everyone: an object's id, link or
  starting action, never its motion, which each machine runs. A non-owner draws only the owner's
  awake actors, and a hit on one waits for step 6. The game's own snapshot writes are no longer
  guarded in the game: a record reaches the host's save only from the owner the host names, so
  `coop_floor_records_kept` is gone.
- 2026-10-27: Order 6 done (Hits across machines). Only an enemy's melee is re-judged by its
  victim; projectiles, jumps and blasts are the owner's to judge against the victim's pose, since
  their flight exists only there. Players pass through each other, so one-cell corridors cannot be
  blocked. The last player's aim wins, as the game keeps one. Effects' sounds play as they appear
  and go. A dead player's menu backs out by itself.
- 2026-10-27: Order 7 done (Death). Down and in a fight travel in the pose, not as messages, as
  continuous state; the fight is judged by the player's own machine, the enemies' part of it by
  their action, so the owner's frame carries it. A down player comes back after 30 seconds
  whatever the fight, since an enemy that cannot reach a player can pursue it forever. The wipe
  needs a guard against a guest's messages from before it, and a save point kept in the file.
- 2026-10-27: other players' spells and sounds (Other players' spells and sounds) travel in the
  pose, not in the owner's frame: they are the caster's, wherever the floor runs, and the pose is
  already played in step with the knight. Sounds go as counts, so a lost pose loses none.
- 2026-11-03: multiplayer settings and the name bar (Multiplayer settings). Sending a player away is
  built for private rooms too, refused by the host and for the session rather than by the service.
- 2026-10-27: the profile (The profile): chosen on the title, not in a room, so it travels in
  the hello and the roster rather than every pose. A player's own pack changes are left out of
  its feed, since its game already says so.
