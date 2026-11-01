# Modding

Mods are Lua 5.4 scripts that the game loads, and reloads, while it runs. The
workflow follows [Verdite2](https://github.com/Voicedrew11/verdite2): edit a
file, save it, and the running game picks it up.

This page documents API 0.5, what ships today. Where it is going, API 1.0, is
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
`main.lua`. `require("other")` loads `other.lua` from the same folder
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

## Coop

Mods do not run in coop. While the coop world is in play every mod is paused:
no callbacks, no events, and `kf.play` and `kf.game` values are not applied, so
the game plays as if no mod were set. The Mods panel says so and disables its
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
use. A writable integer may be changed within its range; a value outside it is
an error that stops the mod, as a `kf.player` write is. Nothing is cancellable
yet. The events are listed in `kf.d.lua` too.

| event | fields | when |
|---|---|---|
| `on_damage` | `target` string, `"player"` or `"actor"`; `actor` integer, the actor's slot (-1 for the player); `kind` integer, the actor's definition id (-1 for the player); `amount` integer, writable 0 to 65535, the HP lost (0 for none); `by_player` boolean, the player made the blow (false for the player's own damage) | a blow hits the player or an actor. Hazards, poison and falls are not blows: no event. `amount` is the blow's loss before `kf.play.invincible` ignores it. |
| `on_kill` | `actor`, `kind`, `experience` integer, writable 0 to 32767 (the EXP the player gets; 0 unless the player made the kill, and writes are then ignored), `by_player` boolean | a blow kills an actor |
| `on_level_up` | `level` integer, the new level | the player reaches a level |
| `on_item_pickup` | `item` integer, the item id | the player takes an item from the ground |
| `on_map_load` | `floor` integer (1 to 5), `map_variant` integer | a floor's map loads, including a reload of the same floor |
| `on_floor_change` | `from` integer, `to` integer | the floor loaded differs from the last one loaded (`from` is 0 before any) |
| `on_dialogue` | `floor`, `character`, `stage`, `page` integers | a dialogue page is shown |
| `on_shop` | `bank` integer, the shop's stock bank | a shop opens |
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
| `kf.view()` | `x, y, w, h` of the whole game picture in window pixels (wider than 4:3 in widescreen) |
| `kf.view_original()` | `x, y, w, h` of the original 320×240 area, centred in the picture |
| `kf.player.<field>` | read/write player state (below) |
| `kf.player_fields()` | list of field names |
| `kf.warp(floor [, cell_x, cell_z [, yaw]])` | queued, see below |
| `kf.equipped(slot)` | the item id worn in `"weapon"`, `"head"`, `"body"`, `"arm"`, `"leg"`, `"shield"` or `"accessory"`, or nil |
| `kf.equip(slot [, item])` | queued, see below |
| `kf.save_config()` | as `mod.save_config()` |
| `kf.api_version` | `"0.5"` |

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

`give` and `take` take a named item and `n` of at least 1; a larger `n` is held to a stack.

### Equipment and magic

`kf.equip(slot [, item])` wears a carried item that fits the slot (a helm in `"head"`, a shield in
`"shield"`), or takes it off when `item` is nil. It is queued like `kf.warp`: `kf.equipped` shows the
change from the next update. The pack keeps the item's count, as the equipment menu does. Full Plate
takes the arm and leg armour off, as the menu does. The slots, in the order coop keeps them, are
`"weapon"`, `"head"`, `"body"`, `"shield"`, `"arm"`, `"leg"` and `"accessory"`.

`kf.magic.learned(spell)` and `kf.magic.set_learned(spell, learned)` take a spell from 0 to 8, in the
game's order (the spell menu lists 4 to 8). Unlearning the selected spell deselects it, as the menu does.

### `kf.game`

| field | notes |
|---|---|
| `speed` | number, 0.25–8: game time at that multiple of real time |
| `paused` | boolean. `on_update` does not run while paused, so unpause from `on_draw` |

### `kf.play`

| field | notes |
|---|---|
| `noclip` | boolean: walls do not stop the player; height still follows the floor |
| `invincible` | boolean: HP losses are ignored, so no death |
| `peaceful` | boolean: enemies never attack the player |
| `move_speed`, `turn_speed` | number, 0.25–8 |

Each mod's `kf.play` and `kf.game` values are its own; the game combines them.
A switch is on when any mod turns it on, speeds multiply, and the product is
held to 0.25–8. A mod's values are dropped when it unloads, reloads or fails.

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
| `ui.draw_text(x, y, s [, r, g, b, a [, size]])` | window pixels, drawn over everything; `size` in pixels (default 18) |
| `ui.draw_rect(x, y, w, h [, r, g, b, a [, filled]])` | |
| `ui.game_to_screen(x, y)` → x, y | converts PS1 320×240 coordinates (the original area, where the game's HUD and menus are) |

## Examples

`mods/stathud` (HUD drawing + settings), `mods/quickheal` (hotkey + writing
player state), `mods/debug` (`kf.play`, `kf.game`, `kf.warp`, bookmarks) and
`mods/killcount` (`on_kill` and the `save` table) and `mods/bestiary`
(`kf.actors`, `kf.items`) in this repository. The
build puts them beside the executable, and the game copies each one, disabled,
into the mods folder the first time it sees it; `mods/.examples` lists those
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
has, so no mod waits on the curated layer to grow.

| area | what | |
|---|---|---|
| player | stats (today), position, facing, map and floor, equipment, inventory, magic, status | read/write |
| entities | the actors loaded on the map: kind, HP, position, state; spawn and remove | read/write |
| world | the current map, its cells and walls, doors, chests, triggers, story flags, the automap | read/write |
| game flow | pause, update rate (turbo, slow motion), warp, load a map, the screen shown (menu, shop, save point) | |
| events | `on_damage`, `on_kill`, `on_item_pickup`, `on_floor_change`, `on_map_load`, `on_level_up`, `on_dialogue`, `on_shop`, `on_save`, `on_load_game`, `on_death` | a handler may cancel or change the event where the game allows |
| rules | damage, hit and dodge, EXP to the next level, level-up gains, drops, shop prices, enemy stats at spawn, HP and MP regeneration | overridable functions: the game asks, each mod in load order may change the answer |
| input | the pad, keys and mouse in the game's own terms (actions, not keycodes); press and block actions | |
| assets | textures, models, sounds, music, text | drop files in the mod's `assets/` under the disc's paths, or swap them from Lua at run time |
| drawing | 2D over the picture (today), plus 3D in the world: lines, markers, text at a point, lights | |
| audio | play the game's sound effects and music, or the mod's own | |
| saves | a `save` table stored with each save slot, beside the game's own | |
| other mods | `mods.get(id)` returns another mod's exported table; mods can raise their own events | |
| raw | `kf.game.<global>.<field>`: every named global and struct field in the decomp, typed and bounds-checked | unstable: follows the decomp's renames |

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
`kf.game` promises nothing, as the decomp renames freely.

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
   `kf.game`, `kf.play` and `mods/debug`. Not yet: doors, chests and the automap,
   which the decomp keeps no reveal state for.
3. ~~Events and the per-save `save` table.~~ Done in 0.4 (2026-10-31): the
   events above and `save`. Not yet: nothing is cancellable, and HP lost outside
   blows (hazards, poison, falls) raises no event.
4. Entities, items, equipment and magic done in 0.5 (2026-10-31): kf.actors, kf.items, kf.equip, kf.magic. Rules: next.
5. The generated raw layer.
6. Asset overrides, audio, 3D drawing.
7. Input actions: reading, pressing and blocking them.

### Open questions

- How the raw layer is generated: libclang over the decomp's headers, or a
  listed subset.
- Asset formats: PNG for textures, and how replaced textures sit in the Vulkan
  backend's texture pages, including larger replacements.
- Whether rules that run every update (regeneration) are worth an event per
  call, or are better as values a mod sets.
