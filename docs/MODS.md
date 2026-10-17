# Mods

The runtime-loaded mods under `mods/`: what each does, how it is built, and what
has been measured or judged. The addresses they rest on are in
`docs/GAME_INTERNALS.md`; this document is about the mods themselves.

A mod is a folder with a `mod.json` and C# that the runtime compiles with Roslyn
at boot (no implicit usings; every namespace named). It can reference the port's
own types (`Kf1.*`) and the recompiled code (`Recompiled.KingsField1_game`),
since the compiler references every loaded assembly. Mods default to off: enable
one in the game's Mods panel, which writes `mods.<id>.enabled=True` under
`[RecompOne]` in `interface.ini`. The compile cache is `mods/.cache/` (ignored).

## Debug Tools (`mods/kf1debug`, `kf1.debug`)

Built 2026-10-17 as a port of Verdite3's `mods/kf3debug`: the same structure,
class names and conventions, with every address re-found on this disc. Everything
is off until it is turned on, and nothing acts without a character in an area
(`GameState.IsInGame`: GAME.EXE is the overlay and max HP is non-zero), so the
opening and the title are untouched.

| file | what |
|---|---|
| `GameState.cs` | the player block; stage A `func_80018880` is the player's tick, and every feature hooks it. `SetPosition` writes the floor (y + 1500) and the cell bytes with the last cell equal, so a move never fires an exit |
| `Cheats.cs` | invincibility, infinite MP, enemies that ignore you, the walk/turn speed multiplier |
| `Noclip.cs` | flight through walls, camera-relative, held to the 100 × 100 cell map; the cinematic camera (which sets `ViewSmoothing.LeadSuppressed`, as Verdite3's does); return to start; snap to floor |
| `Attributes.cs` | EXP, level (through the game's level-up), HP/MP and maxima, gold, STR, MAG, the practice counters and charge gauges, the condition bits and six timers, and the derived ratings with a lock |
| `Items.cs` | the 80 count bytes (give, remove, set, give all, clear) and the weapon and six armour slots through the game's setters |
| `Magic.cs` | the nine player spells: learn, forget, learn all, and hold (records 4-8) |
| `Warp.cs` | four bookmarks, persisted with their area, coordinates, and a warp to areas 1-5 |
| `Hotkeys.cs` | F2 panel, F3 noclip, F4 invincibility, F5/F6 bookmark 1, F7 snap to floor, F8 back to the noclip start; V or the pad's spare button is film mode (cinematic camera, enemies ignoring you and noclip, together); Page Up/Down fly, Left Shift is fast |
| `DebugPanel.cs` | the dockable panel (Debug ▸ KF1 Debug): Cheats, Attributes, Items, Magic, Warp, State, Keys |

### The hooks

| address | kind | by | why |
|---|---|---|---|
| `0x80018880` stage A | pre | Warp | the area transition and the placement, before the tick reads the position (where `patches/Warp.cs` runs the shell's warp) |
| `0x80018880` stage A | post | all | the last player code before stage B copies the player into the camera: flight, restores, queued edits, hotkeys |
| `0x80016324` player hit | pre | Cheats | skipped while invincible: no HP, no condition, no flinch. No caller reads its return |
| `0x8001564C` HP add | pre | Cheats | a negative delta (the drains, the poison tick) clamped to 0 |
| `0x80015164` death | pre | Cheats, Noclip | refused while invincible or flying; the 0x52 hazard cell calls it at full HP |
| `0x8002E2E8` behaviour picker | pre | Cheats | the distance set to `0x20000`, past every u16 range |
| `0x800171FC` collision move | pre | Cheats | the walk's distance scaled; only stage A calls it (three sites) |
| `0x80015714` recompute | post | Attributes | the derived words held while locked |

The rate words `0x80057E68`/`0x80057E70` cannot be scaled as Verdite3 scales its
own: stage A writes them at `0x80018A60`–`0x80018A80` and spends them inline,
with no call between. So the walk is scaled at the collision move, and the turn
by adding `(multiplier - 1) × turnVel` to yaw after stage A.

Calls the mod makes into the game: `func_80036850(area, 0)` (warp, after
`Interrupts.Poll`, as `patches/Warp.cs`), `func_80016058(0)` with EXP topped up to
the threshold (one level a call), `func_80015714` (recalculate), `func_80016A30`
(weapon), `func_80016848(id, slot)` (armour), `func_800167E4` (hold a spell).
There is no give-item routine found, so a give writes the count byte, capped at
99 as the shop caps it, and learning writes the record's +0 byte.

The equip queue keeps the menu's one rule: slot 1 = id 21 occupies slots 2 and
3, so equipping 21 empties them and an equip into either is refused while it is
worn.

### Measured (2026-10-17)

With a throwaway self-test (a file in the mod, driving the panel's queues on a
tick schedule from a New Game, removed afterwards) and the shell:

- **Level up ×3**: level 1 → 4, max HP 30 → 42, max MP 20 → 27, STR 20 → 24,
  MAG 20 → 22, next 285; HP and MP not refilled, as the game does.
- **Give all**: 67 given, 69 of 80 held; the 11 unused ids (by their `FFFF FFFF`
  name records) are exactly 4, 10, 17, 23, 57, 58, 63, 64, 69, 70, 71.
- **Learn all**: 8 learned, 1 already (record 8); **hold 5**: `0x800A07DC` = 5.
- **Equip**: weapon 3, armour 0 = 13, armour 1 = 21 landed; armour 2 = 32 was
  refused by the id-21 rule.
- **Invincibility and infinite MP**: HP poked to 1 and MP to 0 read 42/42 and
  27/27 a tick later; HP poked to 0 was refilled before stage A's HP == 0 test,
  state stayed 0.
- **Area warp** 1 → 2: arrived at (31000, -11500, 5000) on the cell's floor with
  noclip on (Fly after warp). **Bookmark** taken in area 1, recalled from area 2:
  warped back and placed at (31000, -11500, 4000).
- **Noclip** (6000 a second, the shell's `stick`): one second forward moved z by
  5948, one second right moved x by 6002; the cell and last-cell bytes followed
  (18, 4). The shell's `press` writes the guest's pad buffer, which noclip does
  not read, so `stick` is the way to drive it.
- **Speed multiplier**: one second of stick walk moved z by 3545 at ×1 and 7090
  at ×2.
- **Enemies ignore you**: the picker hook rewrote 100 picks in about a minute in
  area 1.

### What is left to judge by eye

- Whether creatures, with "Enemies ignore you" on, actually leave you alone, and
  what one already attacking does.
- **Which pitch looks down.** With pitch +158 (R2 held), flying forward moved
  the eye down (+y). If that is looking up on screen, "Invert dive" is the fix,
  and the default should flip.
- Page Up/Down and R2/L2 for vertical flight (they read `Controller.State`,
  which the shell cannot drive), and Left Shift/R3 fast.
- The cinematic camera's feel, and that mouse look stays smooth under it.
- A speed multiplier above about ×4 takes longer steps than the collision move
  was written for: whether walls still hold.
- The armour slots' names (helm, body and so on) and the derived ratings' meanings,
  read off the status and equip screens.
