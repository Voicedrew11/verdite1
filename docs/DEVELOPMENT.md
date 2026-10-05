# Development: build, run and diagnose

How to build the recompiler, recompile the game into `generated/`, build and run
`KingsField1`, and read a run's logs. Verdite3's `docs/DEVELOPMENT.md` is the
model, with its `KF3_*` switches becoming `KF1_*` here.

## Status

Boots through OPEN.EXE into GAME.EXE's first area (2026-10-05), driven by
scripted input. Nothing is judged by eye yet.

## Build and run

```bash
bash scripts/setup_tools.sh        # build the recompiler
dotnet run --project tools/RecompOne/RecompOne.Recompiler -c Release --no-build -- config/kf1.json
dotnet build KingsField1Recomp.csproj -c Release
dotnet bin/Release/net10.0/KingsField1.dll disc/KingsField1.cue
```

The recompile reports `applied 42 patches, 0 reimplementations` and 1632
functions. `setup_tools.sh --signatures` fetches the PSY-Q bank `--autoconfigure`
reads; `--pull-fork`/`--push-fork` and `--pull-core`/`--push-core` move the two
subtrees.

Run it from the repository root: the runtime writes `carda.sav`, `cardb.sav`,
`settings.json` and `interface.ini` into the working directory (all gitignored).
**The disc comes from `settings.json`'s `CdPath`, not the command line**: in a
directory with no `settings.json` the runtime opens its disc picker and waits for
a person. A minimal one:

```json
{ "CdPath": "/abs/path/to/disc/KingsField1.cue", "CardAPath": "carda.sav", "CardBPath": "cardb.sav",
  "CardAEnabled": true, "CardBEnabled": true }
```

## Diagnostics

`KF1_LOG=bios,cd,gpu,dma,sdk,spu,mdec,irq` (or `all`) turns on the runtime's log
channels. Every run prints each overlay as it loads:

```
[Dispatcher] loaded overlay: main
[Dispatcher] loaded overlay: open
[KF1] irq callback table: open 0x800423C0
[Dispatcher] overlay open overwritten by game
[Dispatcher] loaded overlay: game
[KF1] irq callback table: game 0x800642F0
```

With `sdk`, the area load ends with `CdRead … lba=2923` (`KF/B1/MIXB.DAT`), and
the frames that follow are `DrawOTag` lines, about 20 a second.

**For a hang, take the managed stack** of the live process; recompiled functions
carry their MIPS address in their name. The pid is the `dotnet` process, not a
`timeout` wrapping it:

```bash
~/.dotnet/tools/dotnet-stack report -p $(pgrep -f '^dotnet bin/Release/net10.0/KingsField1.dll')
```

## The acceptance test

What a change to the fork, the config or the maps must keep passing. Measured
2026-10-05 against fork `8c4e137` and Verdite Core `91f4a4a`, all of it by
program; **nothing has been looked at by eye**.

1. The recompile reports `applied 42 patches, 0 reimplementations`.
2. Boot from the cue: the log shows `open`, its table `0x800423C0`; three
   Starts take the attract to a New Game, and the log shows `game`, its table
   `0x800642F0`, and the area's reads ending at `lba=2923`.
3. `KF1_AUTOSTART=new KF1_AGENT=1`: `[KF1] autostart: in area 1, HP 30/30`, and
   the beacon's `loop` true.
4. Walk: `press Up 1000` on the command channel moves `pos` about 4000 along the
   facing.
5. Change areas: standing on the exit cell (z 11, x 25) of area 1 (a `poke` of
   the position to (51000, -11500, 23000) and a step) loads `KF/B3`; the beacon
   reads `area` 3 and `loop` true again.
6. Save: standing before a save point (a `poke` to (52850, -11500, 73500), yaw 0),
   Circle, Circle: `carda.sav` gains `BISLPS-00017KF`, 5 blocks. A check:

   ```bash
   python3 -c "d=open('carda.sav','rb').read();print([d[i*128+10:i*128+30].split(b'\0')[0] for i in range(1,16) if d[i*128]==0x51])"
   ```
7. Load it from the in-game menu: Cross, Down x5, Circle, Circle, Right,
   Circle; the position becomes the save's.
8. No `unmapped call` anywhere in the log.

**Cross opens the in-game menu and Circle examines**: a scripted Circle in front
of a save point, or a run of them in the save screen, writes card A. Keep a copy
of `carda.sav` before driving the menus.

## Driving the game without a person

Three switches, all off unless set (see `docs/ENV_VARS.md`), Verdite3's harness
on this game's addresses:

- **`KF1_AGENT=1`**, the beacon (`patches/AgentBeacon.cs`): `[KF1-AGENT] overlay
  <name>` on each load, and once a second
  `{"overlay":…,"inGame":…,"loop":…,"hp":…,"maxHp":…,"mp":…,"maxMp":…,"area":…,"pos":[x,y,z],"pitch":…,"yaw":…}`.
  `inGame` is GAME.EXE up and a non-zero max HP; **`loop` is whether the main
  loop's stage A ran in the last second**, false for about four seconds after
  GAME.EXE loads, while the area does. The fields are "The player" in
  `docs/GAME_INTERNALS.md`.
- **`KF1_SHELL=1`** (or a port), the command channel (`patches/AgentServer.cs`):
  TCP `127.0.0.1:27901` (Verdite2 uses 27900 and Verdite3 27903), one request a
  line, one JSON line back: `state`, `press <button> [ms]`, `peek <hex addr>
  [bytes]`, `dump <file>` (the 2 MB of RAM, for diffing), `poke <hex addr> <hex
  bytes>` (a diagnostic: it writes the game's own state), `help`. Everything
  runs on the game thread, from the vblank. A poke of the position
  (`0x800A0824`) can be lost to a stage that already holds the old one; check
  `state` after it.
- **`KF1_AUTOSTART=new`** (`patches/AutoStart.cs`): Start is pulsed through
  OPEN.EXE until GAME.EXE loads, then released; a Start that lands in the area
  opens the in-game menu. Measured: area 1 at HP 30/30, the loop turning, about
  20 s after boot.

```bash
SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054C/0x0CE6 KF1_AUTOSTART=new KF1_AGENT=1 KF1_SHELL=1 \
    dotnet bin/Release/net10.0/KingsField1.dll disc/KingsField1.cue
# [KF1] autostart: booting into a New Game
# [KF1] autostart: in area 1, HP 30/30
```

**`KF1_AUTOPAD=seconds:button:holdMs,…`** (`Program.cs`): scripted pad input
through the BIOS pad read (`PadReadEvent`), its clock started by GAME.EXE's load,
or by OPEN.EXE's with `KF1_AUTOPAD_FROM=open`. From boot to the first area:

```bash
SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054C/0x0CE6 KF1_AUTOPAD_FROM=open \
KF1_AUTOPAD=8:Start:300,11:Start:300,14:Start:300 \
    dotnet bin/Release/net10.0/KingsField1.dll disc/KingsField1.cue
```

The `SDL_` variable hides a DualSense, which stalled Verdite3's scripted boot.
