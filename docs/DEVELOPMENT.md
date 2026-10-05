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

## Driving the game without a person

**`KF1_AUTOPAD=seconds:button:holdMs,…`** (`Program.cs`): scripted pad input
through the BIOS pad read (`PadReadEvent`), its clock started by GAME.EXE's load,
or by OPEN.EXE's with `KF1_AUTOPAD_FROM=open`. From boot to the first area:

```bash
SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054C/0x0CE6 KF1_AUTOPAD_FROM=open \
KF1_AUTOPAD=8:Start:300,11:Start:300,14:Start:300 \
    dotnet bin/Release/net10.0/KingsField1.dll disc/KingsField1.cue
```

The `SDL_` variable hides a DualSense, which stalled Verdite3's scripted boot.
