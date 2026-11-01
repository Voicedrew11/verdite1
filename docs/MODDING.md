# Modding

Mods are Lua 5.4 scripts that the game loads, and reloads, while it runs. The
workflow follows [Verdite2](https://github.com/Voicedrew11/verdite2): edit a
file, save it, and the running game picks it up.

This page documents API 0.9, what ships today. Where it is going, API 1.0, is
planned under [The plan](#the-plan-api-10). The host (discovery, reload, the
sandbox, budgets, `config`, `save`, `mod.*`, events) is libverdite's
`verdite/mods.h`; KF1's tables are rows in `port/ext/mods.cpp`.

## Where mods live

`mods/<folder>/` in the user folder (F1 → Mods → **Open mods folder**):
`%APPDATA%\Verdite\Verdite1` on Windows,
`~/.local/share/Verdite/Verdite1` on Linux.

```
mods/stathud/mod.json
mods/stathud/main.lua
```

`mod.json`:

```json
{
  "id": "stathud",
  "name": "Stat HUD",
  "version": "1.0",
  "author": "you",
  "description": "Shown in the Mods panel.",
  "entry": "main.lua"
}
```

Only `main.lua` is required; `id` defaults to the folder name and `entry` to
`main.lua`. `"default_enabled": true` starts a mod enabled the first time the
game sees it (the run and the map ship this way); after that its switch is
stored like any other. `"enhancement": true` marks a mod that changes the game
from the original: Classic mode holds it, loaded but with no callback or event
until Classic mode is off, and the Mods panel says "off in Classic mode". A held
mod's `kf.play` and `kf.game` values do not apply. `require("other")` loads `other.lua` from the same folder
(`require("lib.util")` loads `lib/util.lua` or `lib/util/init.lua`). Two
folders with the same `id`: the second is listed with an error and never runs.

## Editor support

At start the game writes `mods/kf.d.lua`, a Lua Language Server `---@meta`
file listing every callback, table, function and field with its type, made
from the same tables as the bindings. The example mods carry a `.luarc.json`
pointing at it (`"workspace.library": ["../kf.d.lua"]`); VS Code's Lua
extension, Neovim and others then complete names and flag mistakes.

## Live reload

* The game scans the mods folder twice a second. A new folder appears in the
  Mods panel immediately; mods start **disabled**, like Verdite2.
* Saving any `.lua` file or `mod.json` in an enabled mod reloads it: `on_unload`
  runs, the Lua state is discarded, and the mod starts fresh.
* An error stops only that mod, as does going over its budget (below). The message and stack trace go to the Console
  (F1 → Console) and appear under the mod in the Mods panel. Fix the file and
  save; it reloads on its own.
* The enabled flag and `config` are stored in `settings.json` next to the mods folder;
  `save` is stored with each save slot (below).

## Mods in the settings

A mod the game ships can have rows in the settings, so Setup and the game's
own menu switch it: `mods.<id>` is its enabled switch (`mods.run`, the Run key
on the Controls page; `mods.map` on the Map page), and `mods.<id>.<key>` a
boolean in its `config` (`mods.map.minimap`), set as the mod's own write would
be. The rows show while the mod is in the mods folder.

## Coop

Mods do not run in coop. While the coop world is in play every mod is paused:
no callbacks, no events, and `kf.play`, `kf.game` and `kf.rules` values are not
applied, so the game plays as if no mod were set. The Mods panel says so and disables its
switches and Reload all. The count in the overlay's status line reads 0. Leaving
the room resumes the mods where they were; their enabled flags are untouched. A
mod's `save` table belongs to the single-player game and comes back with it.

## Callbacks

Define any of these as globals:

| function | when |
|---|---|
| `on_load()` | after the script runs, on every (re)load |
| `on_update()` | once per game update, when the game hands over its picture (gameplay, menus, cutscenes); not per displayed frame, which smoothing makes more frequent. Read and change game state here. `on_frame()`, its 0.1 name, still works |
| `on_draw()` | every presented frame, for drawing over the game with `ui.draw_*` or a `ui.begin_window` |
| `on_settings()` | while the mod's **Settings** node is open in the Mods panel |
| `on_unload()` | before a reload or when the mod is disabled |

The game is paused while the F1 overlay is open (Settings → Interface), so
`on_update` does not run then; `on_draw` still does.

### Events

The game raises these. A global function of the same name gets one table of
the event's fields. Handlers run inline, at the point in the game where the
thing happens, on the game's thread, so they see the state the game is about to
use. A writable integer may be changed within its range, and a writable
boolean to `true` or `false`; any other value is an error that stops the mod,
as a `kf.player` write is. An event with a writable `cancel` is cancelled when
a handler leaves it `true`; a later mod sees it cancelled and may set it back.
The events are listed in `kf.d.lua` too.

| event | fields | when |
|---|---|---|
| `on_damage` | `target` string, `"player"` or `"actor"`; `actor` integer, the actor's slot (-1 for the player); `kind` integer, the actor's definition id (-1 for the player); `amount` integer, writable 0 to 65535, the HP lost (0 for none); `by_player` boolean, the player made the blow (false for the player's own damage); `cause` string, `"blow"`, `"poison"` or `"equipment"` | the player or an actor loses HP. A `"blow"` is any hit that takes HP, including the pitfall's spikes (a poison hole only poisons, and its poison's ticks are `"poison"`); `"poison"` is the poisoned player's tick, 1 HP every 20 updates; `"equipment"` is worn armour's drain, 1 HP each time its interval comes round (of the armour, only the Blood Mask drains: every 30 updates). A fall deep enough to kill loses no HP: it raises only `on_death`. `amount` is the loss before `kf.play.invincible` ignores it. |
| `on_kill` | `actor`, `kind`, `experience` integer, writable 0 to 32767 (the EXP the player gets; 0 unless the player made the kill, and writes are then ignored), `by_player` boolean | a blow kills an actor |
| `on_drop` | `actor`, `kind`; `gold` integer, writable 0 to 65535 (0 for none); `item` integer, writable -1 to 79, the item that falls (-1 for none) | a dying actor reaches the point of its death animation where it drops (one with no death animation drops nothing), once per death, after the game's own rolls: `gold` and `item` are what the game would drop, and what the handlers leave is what falls. A placed monster's `item` is its placement's item (-1 when it has none), which always falls; a respawning monster's is its kind's item when the drop roll passed, -1 when it failed. An item set where the game dropped none falls as a kind's drop does. Item 39, the gold coin, falls as nothing: give gold with `gold`. The floor 5 boss and the transforming floor 4 monster drop nothing, so raise no event. |
| `on_level_up` | `level` integer, the new level | the player reaches a level |
| `on_item_pickup` | `item` integer, the item id; `cancel` boolean, writable | the player chooses to take an item from the ground or a container and has room for it, before it goes into the pack (`kf.items.count` does not count it yet). Cancelled, it is as if the player had chosen Cancel: the item stays where it lies and can be taken again. |
| `on_map_load` | `floor` integer (1 to 5), `map_variant` integer | a floor's map loads, including a reload of the same floor |
| `on_floor_change` | `from` integer, `to` integer | the floor loaded differs from the last one loaded (`from` is 0 before any) |
| `on_dialogue` | `floor`, `character`, `stage`, `page` integers | a dialogue page is shown |
| `on_shop` | `bank` integer, the shop's stock bank; `cancel` boolean, writable | a shop is about to open, after the shopkeeper's dialogue. Cancelled, the shop does not open, and the shopkeeper goes back to standing. |
| `on_death` | none | the player dies, before the revival or restart |
| `on_save` | `slot` integer: 1 to 3 the game's slots, 4 the quick save, 5 the game set aside in coop | the game is saved, after its save data is written: a change to `save` in this handler goes into the next save |
| `on_load_game` | `slot` integer, as `on_save` | a save is loaded, after its save data is read |

## The sandbox and the budget

A mod gets Lua's `base`, `coroutine`, `table`, `string`, `math` and `utf8`
libraries, without:

- `io` and `os`: files go through `mod.fs`, time through `mod.time()`;
- `package`, C modules and `package.loadlib`: `require` is the host's own;
- `string.dump` and bytecode: `load` reads text only, as do `loadfile` and
  `dofile`, which reach only the mod's folder;
- `debug`, except `debug.traceback`.

`print` writes to the console as `mod.log` does.

Each callback, and the script's first run, may execute 100 million Lua
instructions; each mod may hold 64 MiB. Going over either stops the mod with
an error, even when the mod catches it with `pcall`, so a loop that never ends
or a table that never stops growing cannot hang or exhaust the game.

## `mod`

Given by the host to every mod, in every game.

| call | returns |
|---|---|
| `mod.id`, `mod.api_version` | the mod's id; the API version |
| `mod.log(...)` | prints `[id] ...` to the console |
| `mod.time()` | seconds since the game started |
| `mod.save_config()` | saves `config` when the callback returns |
| `mod.fs.read(path)` | the file's bytes, or `nil, reason` |
| `mod.fs.write(path, text)` | `true`, or `nil, reason`; makes the folders; not `.lua` files or `mod.json`, which would reload the mod |
| `mod.fs.exists(path)` | boolean |
| `mod.fs.list([folder])` | the names in a folder, sorted, folders ending in `/` |

Paths are relative to the mod's folder; `..`, an absolute path or a link that
leads outside it is refused.

## `config`

A table that persists per mod. Values may be numbers, booleans, strings or
nested tables with string keys. It is saved when a `ui.*` widget changes, when the mod unloads, or when a
callback that called `mod.save_config()` (or `kf.save_config()`) returns.

```lua
config.speed = config.speed or 1.5          -- default on first run
function on_settings()
  config.speed = ui.slider_float("Speed", config.speed, 0.5, 3)
end
```

## `save`

A table kept with each save slot, in the game's save folder: `slot1.mods.json`,
`slot2.mods.json` and `slot3.mods.json` beside the slots, `quick.mods.json`
beside the quick save, and `aside.mods.json` beside the game set aside in coop.
Its values are those of `config`, and each mod has its own part, by id.

It is written when the game saves, and read when a save loads, before
`on_load_game`. It is cleared when a new game starts, including the restart
after a death without the Dragon King grass fruit; a revival keeps it. A mod that
is disabled keeps its part in the save, and gets it back when enabled.

A load replaces the table, so read `save.x` each time rather than keeping the
table in a local:

```lua
function on_kill(kill)
  if kill.by_player then
    save.kills = save.kills or {}
    save.kills[tostring(kill.kind)] = (save.kills[tostring(kill.kind)] or 0) + 1
  end
end
```

## `kf`

| call | returns |
|---|---|
| `kf.log(...)` | prints `[id] ...` to the console and `kf.log` |
| `kf.context()` | `"opening"`, `"gameplay"`, `"menu"` or `"scripted"` |
| `kf.frame()` | presented-frame counter |
| `kf.time()` | seconds since start |
| `kf.key_down(name)` / `kf.key_pressed(name)` | SDL key names: `"F5"`, `"G"`, `"Left Shift"`; `pressed` is true on the first frame only. Both are false while the overlay is open. |
| `kf.action_down(name)` / `kf.action_pressed(name)` | an action held by any of its keys, pad buttons or touch controls (below); `pressed` is true on the first update in `on_update`, the first drawn frame in `on_draw`. Both are false while the overlay is open. |
| `kf.action_hold(name, on)` | while `on` is true, the game sees the action held, as if its key were down |
| `kf.action_press(name)` | the game's next button read sees the action held, and the one after released: one press |
| `kf.action_block(name, on)` | while `on` is true, the game does not see the action from the player's keys, pad or touch; a blocked movement or look direction also stops that direction of the stick and the mouse |
| `kf.view()` | `x, y, w, h` of the whole game picture in window pixels (wider than 4:3 in widescreen) |
| `kf.view_original()` | `x, y, w, h` of the original 320×240 area, centred in the picture |
| `kf.player.<field>` | read/write player state (below) |
| `kf.player_fields()` | list of field names |
| `kf.warp(floor [, cell_x, cell_z [, yaw]])` | queued, see below |
| `kf.equipped(slot)` | the item id worn in `"weapon"`, `"head"`, `"body"`, `"arm"`, `"leg"`, `"shield"` or `"accessory"`, or nil |
| `kf.equip(slot [, item])` | queued, see below |
| `kf.save_config()` | as `mod.save_config()` |
| `kf.api_version` | `"0.9"` |

The actions are `forward`, `backward`, `strafe_left`, `strafe_right`,
`turn_left`, `turn_right`, `look_up`, `look_down`, `attack`, `magic`,
`interact`, `inventory`, `pause`, `pause_or_back`, `confirm`, `back`, `run`
(Shift, the left stick's click, the touch Run button), `quick_save`,
`quick_load`, `compare_language` and `full_map` (M, the touch Map button).
`run` and `full_map` mean nothing to the game: they are there for mods. An
action's press in `on_draw` is seen while the game is paused, so a mod that
pauses can unpause on one.

`kf.action_down` and `kf.action_pressed` read the player's own controls,
without any mod's holds or blocks, so a mod can block `attack` and still read
the attack key. The holds and blocks of all mods add up, a hold wins over a
block, and a mod's go when it unloads or fails, and in coop. A blocked
`quick_save` or `quick_load` does nothing; a pressed one acts as the key does.

`kf.player` fields (integers): `hp max_hp mp max_mp gold experience next_level_experience
level physical_power magic base_physical_power base_magic
physical_power_training magic_training magic_charge status_flags
cutting_attack striking_attack piercing_attack holy_attack fire_attack
cutting_defense striking_defense piercing_defense poison_resistance
magic_defense fire_defense curse_timer darkness_timer poison_timer
slowed_timer`, plus read-only `floor`. Writes go straight into the game's
`player_state`; the game may recompute derived stats later. A value outside
the field's range (a negative `gold`, an `hp` over 65535) is an error, not
truncated. `status_flags` is bits: 1 curse, 2 darkness, 4 poison, 8 slowed,
16 fire defense boost.

Position: `x`, `y`, `z` are read-only world units, 2000 per cell; `y` is the
eye height. `cell_x`, `cell_z` and `map_variant` are read-only, and so is
`highest_floor`. `yaw` (0–4095, wraps) and `pitch` (−191..191) are read/write.

### `kf.warp`

`kf.warp(floor [, cell_x, cell_z [, yaw]])` is queued and runs between game
updates. To another floor it loads that floor (floor 5 by its entry), then
moves the player to the cell, or to the first open cell when none is given. On
the same floor it only moves. A floor outside 1–5, a cell off the map, or a
blocked cell on the current floor is an error at the call; a cell on another
floor that turns out blocked once it loads falls back to the first open cell.

### `kf.world`

| field | notes |
|---|---|
| `floor`, `map_variant` | read-only |
| `cell_blocked(x, z)` | boolean |
| `floor1_passage_opened`, `floor1_revival_enabled`, `floor5_character_arrived`, `floor5_weapon_transformed`, `floor5_boss_encounter_started`, `floor5_boss_defeated` | story flags, booleans, read/write |
| `floor3_revealed_pieces` | story flag, integer 0–4, read/write |

### `kf.actors`

The monsters on the floor. A slot is *listed* when it holds an actor that is not disabled. Every call
on an unlisted slot is an error, except `get`, which returns nil. The calls work in gameplay or a
menu with a floor loaded, and are an error anywhere else.

| call | returns |
|---|---|
| `kf.actors.list()` | the listed actors, in slot order: `{slot, kind, hp, x, y, z, yaw, cell_x, cell_z, action}` |
| `kf.actors.get(slot)` | that table, or nil |
| `kf.actors.kinds()` | the kinds placed on this floor: `{kind, hp, experience, gold, drop_item}`, `drop_item` -1 for none |
| `kf.actors.set_hp(slot, hp)` | HP 1 to 65535; not on an actor that is dying (kill takes one down) |
| `kf.actors.move(slot, x, z [, yaw])` | to a world position in an open cell, at the floor's height |
| `kf.actors.kill(slot)` | dies as the game kills: its death animation and drops play; no experience, no `on_kill` |
| `kf.actors.remove(slot)` | vanishes with no death and no drop; its slot and cell are free again |
| `kf.actors.spawn(kind, x, z [, yaw])` | the new slot, or nil when the pool is full; a kind not placed on this floor is an error |

`kind` is the monster's definition id; it means a different monster on each floor. `action` is
`"idle"`, `"wander"`, `"pursue"`, `"retreat"`, `"attack"`, `"hurt"`, `"dying"` or `"other"`. Positions
are world units, 2000 to a cell; `yaw` is 0 to 4095.

A killed monster stays listed, as `"dying"`, after its death animation: the game keeps it on the floor
until the player is out of range, and `remove` takes it out. A monster that has not woken reads hp 0,
and wakes at its definition's full HP, which overrides `set_hp`.

```lua
local kinds = kf.actors.kinds()
if #kinds > 0 then kf.actors.spawn(kinds[1].kind, kf.player.x, kf.player.z + 4000) end
```

### `kf.items`

The player's pack: each item id from 0 to 79 holds up to a stack of 99.

| call | returns |
|---|---|
| `kf.items.count(item)` | how many the pack holds |
| `kf.items.give(item, n)` | adds `n` of a named item, up to a stack; the new count |
| `kf.items.take(item, n)` | takes `n`, down to 0; the new count |
| `kf.items.name(item)` | the item's English name, or nil for an id with none |
| `kf.items.description(item)` | what it does, as the item menu says in English (`"Heals 25 HP."`), or nil |

`give` and `take` take a named item and `n` of at least 1; a larger `n` is held to a stack.

### Equipment and magic

`kf.equip(slot [, item])` wears a carried item that fits the slot (a helm in `"head"`, a shield in
`"shield"`), or takes it off when `item` is nil. It is queued like `kf.warp`: `kf.equipped` shows the
change from the next update. The pack keeps the item's count, as the equipment menu does. Full Plate
takes the arm and leg armour off, as the menu does. The slots, in the order coop keeps them, are
`"weapon"`, `"head"`, `"body"`, `"shield"`, `"arm"`, `"leg"` and `"accessory"`.

`kf.magic.learned(spell)` and `kf.magic.set_learned(spell, learned)` take a spell from 0 to 8, in the
game's order (the spell menu lists 4 to 8). Unlearning the selected spell deselects it, as the menu does.

### `kf.automap`

The floor as the automap sees it, published once per update, and what the
player has seen of it. Cells are one unit: x grows east, z north, and cell
(x, z) spans x to x+1. Read-only.

| field or call | returns |
|---|---|
| `floor` | 1 to 5; 0 when no floor is loaded |
| `owns_map` | boolean: the Keeper's or the Magician's map is in the pack |
| `serial` | counts up on every publish |
| `player_x`, `player_z` | numbers, in cells |
| `player_yaw` | radians: 0 faces north, increasing turns west |
| `cell(x, z)` | `"void"` (nothing built), `"wall"`, `"floor"`, a diagonal half (`"half_x_ge_z"`, `"half_sum_le"`, `"half_z_ge_x"`, `"half_sum_ge"`, named by where the walkable half is in the cell's own 0..1 x and z), `"door"`, `"door_open"`, `"chasm"` (walkable, but only by a fatal fall), `"harp_bridge"` (lowered), `"harp_bridge_raised"` |
| `hazard(x, z)` | `"none"`, `"pit"`, `"poison"` or `"bottomless"` |
| `seen(x, z)` | boolean: seen on this floor; kept with each save slot and the quick save, forgotten by a new game |
| `markers()` | a list of `{kind, label, items, x, z}`, at points in cells |

Off the map a cell is `"void"`, with no hazard, and not seen, so a drawing loop
can look past the edge. A marker's `kind` is `"door"`, `"locked_door"`,
`"warp"`, `"save_point"`, `"fountain"`, `"npc"`, `"item"`, `"chest"`, `"enemy"`
or `"harp_bridge"` (the middle of one). `label` is the warp's destination (1–5
a floor, 10 a teleport on the same floor, 11 the way out), 1 for a full
fountain, the person's character id, or a lock's link for a locked door or
chest (255 when open; below 128, the item id of the key it takes). `items` is
an item's id, a chest's contents or an enemy's drop. Items, chests and enemies
are listed whether or not they have been seen: a map that shows them is
cheating.

### `kf.online`

`kf.online.players()` lists the other players in the online room as
`{floor, x, z, yaw}`, in cells and radians as `kf.automap`. Empty offline.

### `kf.game`

| field | notes |
|---|---|
| `speed` | number, 0.25–8: game time at that multiple of real time |
| `paused` | boolean. `on_update` does not run while paused, so unpause from `on_draw` |
| `music_while_paused` | boolean: while this mod pauses, the music plays on. Another mod's pause without it stops the music |
| `mouse` | boolean: the mod takes the mouse. The cursor shows, and the game gets no mouse input (no look, no attack) |

### `kf.play`

| field | notes |
|---|---|
| `noclip` | boolean: walls do not stop the player; height still follows the floor |
| `invincible` | boolean: HP losses are ignored, so no death |
| `peaceful` | boolean: enemies never attack the player |
| `move_speed`, `turn_speed` | number, 0.25–8 |
| `top_speed` | number, 0.25–8: the walking speed limit, as a run raises it (`mods/run` sets 1.6). Not while slowed |

Each mod's `kf.play` and `kf.game` values are its own; the game combines them.
A switch is on when any mod turns it on, speeds multiply, and the product is
held to 0.25–8. A mod's values are dropped when it unloads, reloads or fails.

### `kf.rules`

KF1 keeps its rules as tables: each floor's monster definitions, the level
growth table, the shops' prices, and the weapon and armour records (the only
regeneration there is). So a rule is a value a mod sets, not a function the game
calls. The game keeps its own numbers and lays each running mod's values over
them; per field, the mod later in load order (the Mods panel's order, by name)
wins. A mod's values are laid when the callback that set them returns, and again
each time the game reloads a table (a floor's definitions at every map load, the
prices when the language changes). They come off, and the game's numbers come
back, when the mod is disabled, reloads, fails or calls `kf.rules.clear()`, and
while the mods are paused in coop. Damage stays `on_damage`'s and drops
`on_drop`'s; a rule changes the numbers those events start from.

| call | |
|---|---|
| `kf.rules.set_monster(floor, kind, values)` | a monster kind's numbers on a floor (1 to 5; `kind` 0 to 11, as `kf.actors` names it) |
| `kf.rules.set_level(level, values)` | what reaching a level gives, 1 to 40 |
| `kf.rules.set_price(item, shop, values)` | an item's prices (0 to 79) in a shop: its stock bank as `on_shop` reports it, 1 or 2 |
| `kf.rules.set_regen(item, values)` | a weapon's (0 to 12) or armour's (13 to 38) intervals; accessories have none |
| `kf.rules.clear()` | drops the calling mod's values |
| `kf.rules.original_monster(floor, kind)` | the game's own numbers, shaped like `set_monster`'s values; nil unless that floor is the one loaded |
| `kf.rules.original_level(level)`, `original_price(item, shop)`, `original_regen(item)` | the same for the other tables; nil before the game has loaded the table |

Every field of `values` is optional. A second call for the same record merges
its fields over the mod's earlier ones. A field that does not exist, a value
that is not an integer or one outside the field's range is an error at the
call. `kf.actors.kinds()` shows the numbers in play, rules laid; scale from
`original_*`, so a factor applied again never compounds.

`set_monster`:

| field | range | |
|---|---|---|
| `hp` | 1–65535 | the HP a monster wakes with; one already awake keeps its HP |
| `experience` | 0–32767 | the EXP the player's kill gives, before `on_kill` |
| `gold` | 0–65535 | the gold it drops is a random amount below this |
| `drop_item` | -1, 0–79 | the item a respawning monster drops when its roll passes (a placed one drops its placement's); -1 none. 39, the gold coin, is refused: set `gold` |
| `drop_chance` | 0–255 | the roll passes when a random 0–255 is at most this |
| `attack = {cutting, striking, piercing}` | 0–999 | its blows' attack |
| `defense = {cutting, striking, piercing, holy, fire}` | 0–999 | its defense against the player's blows and spells |

`set_level`:

| field | range | |
|---|---|---|
| `max_hp`, `max_mp` | 1–9999, 0–9999 | the maximums on reaching the level |
| `power_step`, `magic_step` | 0–999 | added to the base physical power and magic on reaching it |
| `experience` | 1–99999 | the total EXP at which the next level comes |

Level 1 is a new game's start: its maximums, the starting powers and the EXP
for level 2. Past level 40 each level adds the difference between levels 39
and 40, so keep those rising. A level's values change later level-ups only: the
maximums the player has, and the EXP for the next level, were fixed when the
last level came, and are the player's (kept in saves).

`set_price`: `buy` and `sell`, 0–65535, read when the player buys or sells and
when the shop's menu shows them.

`set_regen`: the updates between points, 0–65535, 0 for none. `hp_every`
(weapons and armour) regains 1 HP, `mp_every` (weapons) 1 MP, `hp_drain_every`
(armour) loses 1 HP, raising `on_damage` with cause `"equipment"`. A point comes
when the game's count of updates is a multiple of the interval.

The rule tables are never saved: a save holds the player and the world, so a
save made with a mod's rules loads with the game's numbers when the mod is off.
What the rules did stays, as a `kf.player` write does: the maximums and powers
gained at a level-up, the gold spent, the items dropped. In coop the rules come
off when the coop world comes into play, before its first update; a floor's
snapshot sent to another player carries the definitions, and the receiver keeps
its own.

```lua
-- Every floor's monsters at twice their HP: a floor's numbers are known while it is loaded.
function on_map_load(map)
  for kind = 0, 11 do
    local original = kf.rules.original_monster(map.floor, kind)
    if original and original.hp > 0 then
      kf.rules.set_monster(map.floor, kind, {hp = math.min(65535, original.hp * 2)})
    end
  end
end
```

### `kf.raw`

Every named global and struct field of the decomp, read and written by its
decomp name: `kf.raw.player_state.vitals.current_hp`, `kf.raw.item_stock[0][5]`.
It reaches what the curated tables above do not, at the price of stability:
the names are the decomp's and change when it renames them, and nothing here
is covered by the API's version. Prefer the curated tables where they reach.

| access | does |
|---|---|
| `kf.raw.<global>` | a struct or an array as a proxy; any other value as itself |
| `proxy.<field>`, `proxy[i]` | the same, one step down; arrays index from 0, as the game does |
| `proxy.<field> = v`, `proxy[i] = v` | writes one value: an integer, boolean, number or string |
| `#proxy` | an array's length |
| `pairs(proxy)` | a struct's fields in order, or an array's indices from 0 |
| `kf.raw.copy(proxy)` | the whole struct or array as a table, arrays keyed from 0 |
| `kf.raw.path(proxy)`, `tostring(proxy)` | `"player_state.vitals"` |

A proxy holds its path, not an address, and resolves it again from the global
on every access, so a mod may keep one (`local vitals = kf.raw.player_state.vitals`)
for as long as it likes. Every write is checked before it is stored: against
the field's type (an integer's width and sign, an enum's declared values, a
flags enum's bits, a string's length), its array's bounds, and the range the
game survives, from `port/ext/raw_layer.toml` (a map object's `cell_x` is 0 to
99, the grids' columns). Some fields are read-only, each with its reason in that
file and in `api.d.lua`: the equipped and selected ids, the player's position
and current floor, the model and animation choices, and others a write would
crash the game through. A wrong name, index, kind or range,
or a write to a read-only field, is an error that stops the mod, as a bad
`kf.player` write does. Assigning to a whole struct or array is an error.

Not reachable: raw pointers (reach what one points to through the global that
owns it), the allocators, and fields whose types the decomp's headers do not
describe; `api.d.lua` lists every global, field, range and enum, and the
generator's report (`build/<preset>/generated/raw_layer_report.txt`) lists
what is left out and why.

What a write does is the mod's to manage. The game may overwrite it at its
next update, or leave other state out of step with it. Raw writes and
`kf.rules` reach the same tables (monster definitions, level growth, prices,
equipment records); `kf.rules` lays its values again at the next map load,
over a raw write. Equipment changes through `kf.equip`, and the floor through
`kf.warp`, not through `kf.raw`.

Cost, measured headless: about 0.3 µs to read through the whole path from
`kf.raw` and 0.12 µs to read or write through a kept proxy, so 1,000 accesses
an update take well under a millisecond.

```lua
-- Refill HP when it falls below 10: a kept proxy, read and written every update.
local vitals = kf.raw.player_state.vitals
function on_update()
  if vitals.current_hp < 10 then vitals.current_hp = vitals.maximum_hp end
end
```

## `ui` (Dear ImGui)

Only inside `on_draw` and `on_settings`. Colours are 0–1 floats. Sliders show a
number box for typing a value. Mod drawing is not affected by the overlay's
interface scale.

| call | notes |
|---|---|
| `ui.text(s)`, `ui.text_colored(r, g, b, s)` | |
| `ui.button(label)` → bool | true when clicked |
| `ui.checkbox(label, value)` → value | |
| `ui.slider_int(label, v, min, max)` → v | |
| `ui.slider_float(label, v, min, max [, fmt])` → v | |
| `ui.combo(label, index, {items})` → index | 1-based |
| `ui.separator()`, `ui.same_line()`, `ui.spacing()`, `ui.tooltip(s)` | |
| `ui.begin_window(title [, click_through])` → visible; `ui.end_window()` | always call `end_window` |
| `ui.draw_text(x, y, s [, r, g, b, a [, size]])` | window pixels, with a shadow; `size` in pixels (default 18) |
| `ui.draw_rect(x, y, w, h [, r, g, b, a [, filled]])` | |
| `ui.draw_line(x1, y1, x2, y2 [, r, g, b, a [, thickness]])` | thickness in pixels (default 1) |
| `ui.draw_circle(x, y, radius [, r, g, b, a [, filled [, thickness]]])` | filled unless `filled` is false |
| `ui.draw_polygon({x1, y1, x2, y2, ...} [, r, g, b, a [, filled [, thickness]]])` | 3 to 64 points; filled (the default) must be convex |
| `ui.push_clip(x, y, w, h)`, `ui.pop_clip()` | clips the layer's drawing; the end of `on_draw` pops any left |
| `ui.layer(name)` | `"foreground"` (the default each `on_draw`): over everything; `"background"`: over the picture, under the windows |
| `ui.text_size(s [, size])` → w, h | pixels |
| `ui.mouse()` → x, y | window pixels; far off screen when there is no mouse |
| `ui.mouse_down(button)` → bool | 1 left, 2 right, 3 middle; false while the overlay is open |
| `ui.mouse_wheel()` → steps | this frame, up positive; 0 while the overlay is open |
| `ui.game_to_screen(x, y)` → x, y | converts PS1 320×240 coordinates (the original area, where the game's HUD and menus are) |

## Examples

`mods/stathud` (HUD drawing + settings), `mods/quickheal` (hotkey + writing
player state), `mods/debug` (`kf.play`, `kf.game`, `kf.warp`, bookmarks) and
`mods/killcount` (`on_kill` and the `save` table), `mods/bestiary`
(`kf.actors`, `kf.items`), `mods/hardmode` (`kf.rules`) and `mods/run`
(`kf.action_pressed`, `kf.play.top_speed`: the run key, which the game no longer has
itself) and `mods/map` (`kf.automap`, `kf.online`, the drawing calls, `kf.game.mouse`:
the minimap and the full map on M) in this repository. The
build puts them beside the executable, and the game copies each one into the
mods folder the first time it sees it, disabled unless it says
`default_enabled`; `mods/.examples` lists those
already offered, so an example you delete stays deleted.

## The plan: API 1.0

Decided 2026-10-23: mods stay Lua 5.4. It is small, interpreted (so it runs on
the 3DS and the consoles that forbid a JIT), easy to sandbox, reloads on save,
and modders know it. Luau, WebAssembly, native DLLs and C# were weighed; Lua's
missing types are made up for by a definitions file (below).

### What a mod can reach

Inside the game, everything; outside it, nothing but the mod's own folder. The
API is not a short list of approved knobs: a curated layer covers the common
cases with stable names, and a raw layer reaches every named global the decomp
has, so no mod waits on the curated layer to grow. A mod is meant to be able
to change how the game works, not only its numbers: call the game's functions,
run its own code in place of them, and add content the disc does not have. The
one line the raw layer holds is memory safety (decided 2026-11-01): no value
from Lua reaches the game's memory unchecked, since mods come from strangers.

| area | what | |
|---|---|---|
| player | stats (today), position, facing, map and floor, equipment, inventory, magic, status | read/write |
| entities | the actors loaded on the map: kind, HP, position, state; spawn and remove | read/write |
| world | the current map, its cells and walls, doors, chests, triggers, story flags, the automap | read/write |
| game flow | pause, update rate (turbo, slow motion), warp, load a map, the screen shown (menu, shop, save point) | |
| events | `on_damage`, `on_kill`, `on_item_pickup`, `on_floor_change`, `on_map_load`, `on_level_up`, `on_dialogue`, `on_shop`, `on_save`, `on_load_game`, `on_death` | a handler may cancel or change the event where the game allows |
| rules | EXP to the next level, level-up gains, drops, shop prices, enemy stats, HP and MP regeneration (KF1 has no hit or dodge roll); damage through `on_damage` | values over the game's tables (today): each mod in load order may change a field |
| input | the pad, keys and mouse in the game's own terms (actions, not keycodes); press and block actions | |
| assets | textures, models, sounds, music, text | drop files in the mod's `assets/` under the disc's paths, or swap them from Lua at run time |
| drawing | 2D over the picture (today), plus 3D in the world: lines, markers, text at a point, lights | |
| audio | play the game's sound effects and music, or the mod's own | |
| saves | a `save` table stored with each save slot, beside the game's own | |
| other mods | `mods.get(id)` returns another mod's exported table; mods can raise their own events | |
| functions | call any game function the decomp's headers declare; run a mod's code before, after or in place of one | arguments checked as raw writes are |
| content | new items, monster kinds, floors: the game's fixed-size tables grown so a mod's rows fit beside the disc's | |
| raw | `kf.raw.<global>.<field>`: every named global and struct field in the decomp, typed and bounds-checked | unstable: follows the decomp's renames |

### What a mod cannot do

These limits keep the player's machine and the game process safe, which
matters because mods travel as zip files from strangers:

- No files outside its folder. `io` and `os` are replaced by `mod.fs`, which
  reads and writes only inside the mod's folder, and `mod.time()`.
- No processes, no native code: no `os.execute`, `io.popen`,
  `package.loadlib` or C modules.
- No sockets. Online play goes through the networking layer
  ([NETWORKING_PLAN.md](NETWORKING_PLAN.md)), which can carry a mod's messages.
- No `debug` library beyond `debug.traceback`: the rest escapes the sandbox.
- No pointers. Raw access copies values in and out of a named field, checked
  against the field's type and the array's bounds.
- A budget: each callback gets an instruction count (`lua_sethook`) and each
  mod a memory cap (its own allocator). Going over stops that mod with an
  error, as a Lua error does today, instead of hanging or exhausting the game.

The sandbox and the budgets ship in 0.2; `mod.fs` and `mod.time()` are there.

### Timing

- `on_update()` runs once per game update, where `on_frame` runs now
  (`on_frame` stays as its alias). Game state is read and written here. (0.2)
- `on_draw()` runs once per displayed frame, which smoothing makes more
  frequent than updates; it draws, it does not change game state.
- Events run inline, at the point in the game code where the thing happens,
  on the thread the game runs on, so a handler sees and changes state the game
  is about to use, with no locking.

### Types for editors

`mods/kf.d.lua` is a Lua Language Server `---@meta` file listing every call,
field and event with its type, generated from the same binding tables as the
C++. VS Code, Neovim and others then complete names and flag mistakes. The
example mods ship a `.luarc.json` pointing at it.

### Where the code goes

- `libverdite/include/verdite/mods.h`: the game-agnostic host. Discovery,
  `mod.json`, live reload, the sandbox and budgets, `config`, `save`, `ui`,
  `mod.fs`, the event registry and the binding-table types. KF2 and KF3 get
  modding by linking it.
- `port/ext/mods.cpp`: KF1's bindings only, as tables: a new field, event or
  rule is a new table entry.
- Game code calls the event and rule points through a small header in
  `work/include/kf/platform/`, as `screen.h` does for Discord.
- The raw layer's tables are generated from the decomp's headers at build
  time, so they follow the pin when it moves.

### Stability

`kf.api_version` follows semver from 1.0, and `mod.json` names the API version
it was written for. The curated layer keeps its names within a major version;
`kf.raw` promises nothing, as the decomp renames freely.

### Online play

A mod that changes play (state writes, rules, events it cancels) marks itself
`"gameplay": true` in `mod.json`. Coop requires both players to run the same
gameplay mods, compared by a hash; visual and HUD mods are free. Until then
mods are off in coop (decided 2026-10-31).

### Order

1. ~~Move the host into libverdite. Add the sandbox, budgets, `on_update`, and
   generation of the definitions file.~~ Done in 0.2 (2026-10-31). `ui` stays
   in `port/`, bound as a table like the rest, since ImGui is the platform's.
2. ~~Player position, world and game flow, then a debug mod (noclip, god mode,
   infinite MP, peaceful, speed, warp) in place of Verdite's debug menu.~~
   Done in 0.3 (2026-10-31): position, `kf.warp`, `kf.world`'s story flags,
   `kf.game`, `kf.play` and `mods/debug`. The automap's floor and seen cells
   are read-only in 0.7 (`kf.automap`); doors and chests are not reachable yet.
3. ~~Events and the per-save `save` table.~~ Done in 0.4 (2026-10-31): the
   events above and `save`. In 0.6 (2026-10-31): `on_damage` for poison and
   equipment drain, `on_drop`, and cancelling `on_item_pickup` and `on_shop`.
4. ~~Entities, items, equipment and magic, then rules.~~ Done in 0.5
   (2026-10-31): `kf.actors`, `kf.items`, `kf.equip`, `kf.magic`. Rules done in
   0.6 (2026-10-31): `kf.rules` and `mods/hardmode`.
5. ~~The generated raw layer.~~ Built in 0.9 (2026-11-01); its writes fuzzed
   clean the same day ([RAW_LAYER_PLAN.md](RAW_LAYER_PLAN.md), step 5).
6. Asset overrides, audio, 3D drawing.
7. ~~Input actions: reading, pressing and blocking them. Reading done in 0.7
   (2026-10-31), with `kf.automap`, `kf.online`, the drawing calls,
   `kf.play.top_speed` and `kf.game.mouse` and `music_while_paused`, which
   the run and the map need to be mods.~~ Pressing and blocking done in 0.8
   (2026-11-01): `kf.action_hold`, `kf.action_press`, `kf.action_block`.
8. Calling the game's functions: bindings generated from the same headers as
   the raw layer, each argument checked against its type and range.
9. Hooking and replacing the game's functions: a mod's code before, after or
   in place of a game function. We compile the decomp, so the build can give
   each function a hook point; no patching at run time.
10. Growing the game's tables for new content: more item ids than the disc's
    80 (`port/ext/extra_items.cpp` fills unused ids today, it does not add
    any), more monster kinds a floor, new floors, as patches that raise the
    capacities and keep the disc's rows where the game expects them.

### Open questions

- ~~How the raw layer is generated: libclang over the decomp's headers, or a
  listed subset.~~ Decided 2026-11-01: clang over the patched headers, with a
  short listed file of value ranges and aliases; everything is writable but
  raw pointers and the allocators ([RAW_LAYER_PLAN.md](RAW_LAYER_PLAN.md)).
- Asset formats: PNG for textures, and how replaced textures sit in the Vulkan
  backend's texture pages, including larger replacements.
- ~~Whether rules that run every update (regeneration) are worth an event per
  call, or are better as values a mod sets.~~ Decided 2026-10-31: values. KF1's
  rules are tables (regeneration is an interval on the equipment records), so a
  mod sets numbers and the game lays them over its own; damage stays an event.
