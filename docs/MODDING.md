# Modding

Mods are Lua 5.4 scripts that the game loads, and reloads, while it runs. The
workflow follows [Verdite2](https://github.com/Voicedrew11/verdite2): edit a
file, save it, and the running game picks it up.

This page documents API 0.1, what ships today. Where it is going, API 1.0, is
planned under [The plan](#the-plan-api-10).

## Where mods live

`mods/<folder>/` in the user folder (F1 → Mods → **Open mods folder**):
`%APPDATA%\KingsField\SLPS00017` on Windows,
`~/.local/share/KingsField/SLPS00017` on Linux.

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
`main.lua`. `require("other")` loads `other.lua` from the same folder.

## Live reload

* The game scans the mods folder twice a second. A new folder appears in the
  Mods panel immediately; mods start **disabled**, like Verdite2.
* Saving any `.lua` file or `mod.json` in an enabled mod reloads it: `on_unload`
  runs, the Lua state is discarded, and the mod starts fresh.
* An error stops only that mod. The message and stack trace go to the Console
  (F1 → Console) and appear under the mod in the Mods panel. Fix the file and
  save; it reloads on its own.
* The enabled flag and `config` are stored in `settings.json` next to the mods folder.

## Callbacks

Define any of these as globals:

| function | when |
|---|---|
| `on_load()` | after the script runs, on every (re)load |
| `on_frame()` | once per game update, when the game hands over its picture (gameplay, menus, cutscenes); not per displayed frame, which smoothing makes more frequent |
| `on_draw()` | every presented frame, for drawing over the game with `ui.draw_*` or a `ui.begin_window` |
| `on_settings()` | while the mod's **Settings** node is open in the Mods panel |
| `on_unload()` | before a reload or when the mod is disabled |

The game is paused while the F1 overlay is open (Settings → Interface), so
`on_frame` does not run then; `on_draw` still does.

## `config`

A table that persists per mod. Values may be numbers, booleans, strings or
nested tables with string keys. It is saved when a `ui.*` widget changes inside
`on_settings`, when the mod unloads, or when you call `kf.save_config()`.

```lua
config.speed = config.speed or 1.5          -- default on first run
function on_settings()
  config.speed = ui.slider_float("Speed", config.speed, 0.5, 3)
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
| `kf.save_config()` | writes `config` now |
| `kf.api_version` | `"0.1"` |

`kf.player` fields: `hp max_hp mp max_mp gold experience next_level_experience
level physical_power magic base_physical_power base_magic
physical_power_training magic_training magic_charge status_flags
cutting_attack striking_attack piercing_attack holy_attack fire_attack
cutting_defense striking_defense piercing_defense poison_resistance
magic_defense fire_defense curse_timer darkness_timer poison_timer
slowed_timer`, plus read-only `floor`. Writes go straight into the game's
`player_state`; the game may recompute derived stats later.

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

`mods/stathud` (HUD drawing + settings) and `mods/quickheal` (hotkey + writing
player state) in this repository. Copy them into the mods folder to try them.

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

Today every mod gets `luaL_openlibs`, the full standard library; closing that
is the first step.

### Timing

- `on_update()` runs once per game update, where `on_frame` runs now
  (`on_frame` stays as its alias). Game state is read and written here.
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
gameplay mods, compared by a hash; visual and HUD mods are free.

### Order

1. Move the host into libverdite. Add the sandbox, budgets, `on_update`, and
   generation of the definitions file.
2. Player position, world and game flow, then a debug mod (noclip, god mode,
   infinite MP, peaceful, speed, warp) in place of Verdite's debug menu.
3. Events and the per-save `save` table.
4. Entities and rules.
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
