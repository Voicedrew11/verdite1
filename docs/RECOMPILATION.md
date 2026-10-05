# Recompilation: config, function maps and SDK addresses

How the recompiler turns the disc's MIPS into C#: the overlays in
`config/kf1.json`, the swept function maps under `config/funcmaps/`, and mapping
PSY-Q entry points by address to the runtime's HLE. Verdite3's
`docs/RECOMPILATION.md` is the model, and it documents the traps to expect here
too — data swept as code past the end of the text, false starts from `jal`-shaped
data words, and switch tables read past their end. Which of them apply to this
disc, and what is new, goes below.

## Status

Recompiled 2026-10-05: the boot stub `PSX.EXE`, `OPEN.EXE` and `GAME.EXE`, 1632
functions, `applied 42 patches, 0 reimplementations`.

## What is on the disc

Read with `tools/verdite-core/scripts/inspect_disc.py disc/KingsField1.cue` and
`extract_file.py … --header-only` (2026-10-05). The user's dump came as
`King's Field (Japan).cue/.bin`; it was renamed to `disc/KingsField1.cue` and
`.bin`, with the cue's `FILE` line changed to match. One data track, no audio
tracks. The `.bin` is 30,378,432 bytes, SHA-1
`00bfd94ce99bf214b03bdaa07de99b9ca1466550`. The volume id is `SLPS-00017`.

**There is no `SYSTEM.CNF`.** The BIOS then boots `PSX.EXE` with TCB 4, EVENT
16 and the stack at `0x801FFF00`, and so does the recompiler since the fork's
`0090` (it read `SYSTEM.CNF` unconditionally and stopped). The PS-X EXE headers'
`gp` and stack fields hold text (`"tfr"`, `"fake"`, `"94C "`), not values; the
code sets its own.

| file | LBA | file size | entry pc | text addr | text size |
|---|---|---|---|---|---|
| `PSX.EXE` | 38 | 0x1000 | `0x80010100` | `0x80010000` | 0x800 |
| `GAME.EXE` | 40 | 0x46800 | `0x8003AC5C` | `0x80012000` | 0x46000 |
| `OPEN.EXE` | 181 | 0x26000 | `0x8001AA7C` | `0x80012000` | 0x25800 |

**Two executables and no code modules.** `PSX.EXE` is a loader: its main
(`0x80010028`) calls `_96_init`, then `Load`s and `Exec`s (as a call)
`cdrom:OPEN.EXE;1`, then `cdrom:GAME.EXE;1`, and loops back to OPEN. The two
names are its first 0x24 bytes, and `0x80010224` holds the pointer pair (OPEN
first). Before that its crt0 reads `0x1F802040`, a dev-kit DIP switch, to pick a
stack size; every entry of the table at `0x800101B4` is 8 MB, so the value does
not matter. `OPEN.EXE` (the title, the attract, New Game and Continue) and
`GAME.EXE` both load at `0x80012000`, so they are declared as overlays, each with
`"skip": 2048`. Each entry point is a stub that sets `gp` and jumps to the real
start (`0x80013758` in `open`, `0x8001428C` in `game`). There is no `END.EXE`,
and nothing on the disc is code loaded at run time: the area data under `KF/B1`
to `KF/B5` is textures, models and sequences.

The rest of the disc:
- `KF/B0`: OPEN's own scenes (`L0.`, `MIX*`, `OPEN0.`-`OPEN3.`, `RTBL.`) and the
  ending (`END.`, `ENDG.`).
- `KF/B1`-`KF/B5`: one directory per area, `MIX.TIM`, `MIXA.DAT`, `MIXB.DAT` and
  `SND*.SEQ`; `B5` also has `CHR1`-`3.MIM`.
- `KF/COM`: `COM.DAT`, `MIX.TIM`, `STAT.DAT`, read once at GAME.EXE's start.
- `KF/ENE1`-`5` (enemy TIMs), `KF/ITEM1`-`4` (item TMDs), `KF/WEPON` (weapon
  MIMs), `KF/KAN`, `KF/MAP`, `KF/PRSN`, `KF/TALK`, `KF/TIM` (signs, maps,
  portraits, dialogue; `KF/TIM/ICO1`-`3.TIM` are 16x16 4-bit images, likely the
  memory-card icon).
- `E0.`-`E3.` at the root, `LICENSEJ.DAT`, `COPY.TXT`.

## The function maps

Swept 2026-10-05, then harvested and merged, in Verdite3's order:

```bash
RC="dotnet run --project tools/RecompOne/RecompOne.Recompiler -c Release --no-build --"
$RC --generate-function-file -linear-sweep -disc disc/KingsField1.cue \
    -file GAME.EXE -base 80012000 -skip 800 -out config/funcmaps/game.json
# (OPEN.EXE the same; PSX.EXE at -base 80010000 into main.json)
python3 tools/verdite-core/scripts/add_call_targets.py disc/KingsField1.cue GAME.EXE config/funcmaps/game.json
python3 tools/verdite-core/scripts/merge_branch_spans.py
```

| map | swept | after harvest | after merge |
|---|---|---|---|
| `main` | 8 | | 8 |
| `open` | 664 | 665 | 665 |
| `game` | 935 | 949 | 937 |

**No cut was needed.** Unlike Verdite3's executables, the sweep here stops where
the code does: the last function ends at `0x800354F0` in `OPEN.EXE` and
`0x8005580C` in `GAME.EXE` (a BIOS thunk, then data), and nothing past it was
taken for code. Both texts *start* with read-only data: the first function is at
`0x80013734` in `open` and `0x80014268` in `game`.

`merge_branch_spans` rejoined three functions in `game`: `func_8002FA88` (to
`0x800307FC`, eight starts absorbed), `func_80031CC8` (to `0x800328E0`) and
`func_80038A38` (to `0x8003A244`). Verdite3's check for false starts reached by
fallthrough (a start no code `jal`s or `j`s to, that the code before runs on
into) finds only six alignment `nop`s after a `jr ra` in libgte, which are real
starts. The recompiler's escape scan adds 21 entry points in `game`, at labels
inside merged functions, emitted as a label and also as a callable copy, as in
Verdite3.

## The SDK entry points

`--autoconfigure -sweep-all` (the PSY-Q signature bank, fetched with
`setup_tools.sh --signatures`, gitignored) named 262 of 664 functions in
`OPEN.EXE` and 263 of 935 in `GAME.EXE`. `merge_sdk_names.py --auto <dir>` wrote
453 of those into the maps; 56 it refused because `SdkPatches` would bind them,
and 16 because they matched twice or were not identifiers.

**The two executables link the same library, a fixed distance apart per run of
objects.** Of the names both maps carry:

| `open` span of the names | `game` − `open` | first .. last named |
|---|---|---|
| `0x8001AAAC`-`0x80026654` (83) | `+0x201E0` | `CdSetDebug` .. `SsSetMVol` (libcd, libcdstream, libsnd) |
| `0x80029254`-`0x8002F9DC` (39) | `+0x2022C` | `Snd_setVabAttr` .. `PioCallback` (libsnd, libspu, the DMA callbacks) |
| `0x8002C000`-`0x8002C294` (35) | `+0x209AC` | `SetVertex0` .. `ReadGeomScreen` (libgte) |
| `0x8002C2A0`-`0x8002C8D4` (7) | `+0x1FF8C` | `InitGeom` .. `gteMIMefunc` (libgte) |
| `0x80030030`, `0x800301C0` | `+0x2023C`, `+0x2030C` | `EnterCriticalSection`, `ExitCriticalSection` |
| `0x8003049C`-`0x80034A40` (60) | `+0x2031C` | `SetGraphDebug` .. `sprintf` (libgpu, libetc, libc) |

**This 1994 library predates the bank's `VSync`, `PutDispEnv` and
`DMACallback`**, so those three were found by hand (and agree with the earlier
`kf1-port` attempt's addresses). Each was then checked against its counterpart
in the other executable instruction by instruction, immediates aside:

- `VSync(n)` (`0x800555E0`): `n < 2` calls an inner wait once (`0x80055644`);
  otherwise it calls the inner wait with 0 until the hblanks it returns pass
  `n << 8`. The inner wait opens and enables root counter 1's event
  (`0xF2000001`) the first time, waits for the GPUSTAT interlace bit through the
  pointer at `0x80057E54`, and returns the hblank count since the last call from
  `GetRCnt` (B(03h)). Nine `jal` sites in `game`.
- `PutDispEnv` (`0x80050E68`) lies between the named `GetDrawEnv` and
  `GetDispEnv`, and writes GP1 `0x05` and `0x08`.
- `DMACallback(ch, func)` (`0x8004FC2C`) is what the named `SpuDataCallback`,
  `PioDataCallback` and `CdDataCallback` call with their channel: it stores into
  a 7-slot table at `0x800642D0` and sets the channel's bits in DICR through the
  pointer at `0x80057D1C`.

`patches[]` binds the same 21 entry points Verdite2 and Verdite3 bind, in both
executables: 42. The recompiler reports `applied 42 patches, 0
reimplementations`.

| function | `open` | `game` | how |
|---|---|---|---|
| `VSync` | `0x800352C4` | `0x800555E0` | by hand |
| `CdInit` | `0x8001C390` | `0x8003C570` | signature |
| `CdSync` | `0x8001AB34` | `0x8003AD14` | signature |
| `CdReady` | `0x8001AB54` | `0x8003AD34` | signature |
| `CdControl` | `0x8001ACF8` | `0x8003AED8` | signature |
| `CdControlF` | `0x8001AD5C` | `0x8003AF3C` | signature |
| `CdControlB` | `0x8001ADB8` | `0x8003AF98` | signature |
| `CdGetSector` | `0x8001AE34` | `0x8003B014` | signature |
| `CdReadSync` | `0x8001AB74` | `0x8003AD54` | signature |
| `CdRead` | `0x8001AE54` | `0x8003B034` | signature |
| `DrawSync` | `0x80030578` | `0x80050894` | signature |
| `DrawOTag` | `0x800309D4` | `0x80050CF0` | signature |
| `PutDrawEnv` | `0x80030A2C` | `0x80050D48` | signature |
| `PutDispEnv` | `0x80030B4C` | `0x80050E68` | by hand |
| `StUnSetRing` | `0x8001D194` | `0x8003D374` | signature |
| `StSetStream` | `0x8001D234` | `0x8003D414` | signature |
| `StSetRing` | `0x8001D0EC` | `0x8003D2CC` | signature |
| `StClearRing` | `0x8001D144` | `0x8003D324` | signature |
| `StGetNext` | `0x8001D410` | `0x8003D5F0` | signature |
| `StFreeRing` | `0x8001D308` | `0x8003D4E8` | signature |
| `DMACallback` | `0x8002FA00` | `0x8004FC2C` | by hand |

The disc has no movies, so the `libcdstream` bindings are kept only to match the
siblings. Not bound, as there: the `libgpu` image routines and libpress.

### The interrupt-callback table

`InterruptCallback(irq, func)` (`0x8004FCAC` in `game`, `0x8002FA80` in `open`)
refuses IRQ 3, stores into an 11-slot table and sets `1 << irq` in I_MASK through
the pointer at `0x80057D14`. The table is `0x800642F0` in `game` and
`0x800423C0` in `open`; `Program.cs` sets it per executable. The DMA callback
table sits 0x20 bytes *below* it in this library.
