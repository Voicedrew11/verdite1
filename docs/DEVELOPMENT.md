# Development: building, running and measuring

Everything needed to get the port compiling, running and reporting on itself.
The recompiler's own quirks are in [RECOMPILATION.md](RECOMPILATION.md); the
patches to the RecompOne checkout are in [RUNTIME.md](RUNTIME.md).

**Nothing here builds without the disc** (gitignored, `disc/KingsField2.cue`) or
without `tools/RecompOne` (a gitignored checkout, not a submodule). A `.chd` works
everywhere a `.cue` does — `DiscImage.Open` picks by extension — see "CHD disc
images" in [RUNTIME.md](RUNTIME.md).

## Prerequisites

- .NET SDK 10 — `sudo dnf install dotnet-sdk-10.0`
- `chdman` for CHD conversion — `sudo dnf install mame-tools`

The runtime pulls Silk.NET, SDL, OpenGL and OpenAL via NuGet, all cross-platform
with no Windows-only P/Invoke. Verified working on Fedora 43 / Wayland / Mesa.

## Disc structure

The CHD can be handed to the recompiler and to the game directly. Converting it is
still useful, because the Python tooling under `scripts/` reads cue/bin only:

```bash
chdman extractcd -i "King's Field (USA).chd" -o KingsField2.cue -ob KingsField2.bin
```

Either way it is a single MODE2/2352 track. `python3 scripts/inspect_disc.py` gives the
full listing; the part that matters:

| File | Size | Role |
|---|---|---|
| `SLUS_001.58` | 4 KiB | boot stub named by SYSTEM.CNF |
| `OPEN.EXE` | 186 KiB | title / intro |
| `GAME.EXE` | 378 KiB | the main game |
| `END.EXE` | 166 KiB | ending |
| `CD/COM/*.T` | ~40 MiB | data archives (models, textures, audio, text) |
| `OP/*.S` | ~180 MiB | streamed media |

**The boot executable is a 2 KiB loader stub, not the game.** RecompOne
auto-discovers it from SYSTEM.CNF, so left alone it would recompile 2 KiB and
nothing else. The three real executables **all load at `0x80011000`** and are
mutually exclusive — only one is resident at a time. They are therefore declared
as overlays, which is exactly what the overlay system is for.

Header values come from `scripts/extract_file.py <cue> GAME.EXE --header-only`;
`-base` must be the header's text address rather than an assumed `0x80010000`.

`CD/COM/*.T` and `OP/*.S` are data and streamed media, not code, so they stay out
of the overlay list and are read by the runtime through the CD interface.

## Setting up the tools

```bash
bash scripts/setup_tools.sh
```

Clones RecompOne, applies everything in `patches/recompone/`, and builds the
recompiler. Idempotent, so it is also the way to re-apply local fixes after
pulling upstream.

It gets that idempotency by **peeling the stack off newest-first and then applying
it oldest-first**. Asking each patch on its own "are you already applied?" — which
is what it used to do — only works while no patch touches lines another one added,
and `0010` edits the `GteDepth.cs` that `0009` creates. The symptom was `0009`
reverse-checking against text `0010` had since changed, failing, and being reported
as upstream having moved. `0011`, `0012` and `0014` edit that same file again. Undoing in the opposite
order to applying cannot hit that. A patch that will not reverse stops the peeling
instead of being forced, so a fresh clone peels nothing, and an uncaptured edit
inside the checkout — the normal way a new patch gets written — stops it rather
than being rolled over.

The seventeen patches themselves, and what each one is required for, are in
[RUNTIME.md](RUNTIME.md). Function-map generation — step 2 of the old workflow —
is in [RECOMPILATION.md](RECOMPILATION.md).

## Recompile

```bash
dotnet run --project tools/RecompOne/RecompOne.Recompiler -c Release --no-build -- \
  config/kf2.json
```

2234 functions into `generated/` (~182k lines of C#).

## Build and run

```bash
dotnet build KingsField2Recomp.csproj -c Release
dotnet run --project KingsField2Recomp.csproj -- disc/KingsField2.cue
```

The runtime needs the cue at play time as well as at recompile time; pass it as
the first argument or the runtime will prompt for a disc.

**csproj gotcha:** the RecompOne checkout is nested inside this project
directory, so the SDK's default item globs would compile RecompOne's own sources
and its `obj/` AssemblyInfo files (CS0579 duplicate-attribute errors). Hence the
`<Compile Remove="tools/**" />`. The same trap is `mcp/`, `Verdite2.Launcher/`,
and `packaging/windows/Stub/` (a net48 project whose `obj/` appears the moment
anyone has built the Windows package). Conversely, `generated/` and `patches/`
need *no* explicit include — adding one causes NETSDK1022.


## Diagnostics

Log channels and the port's own probes are all environment variables read in
`Program.cs`; `CLAUDE.md` carries the complete current list. The ones with
something to say about *how* to use them:

- **`KF2_LOG=bios` is very expensive during play.** The game polls `PAD_dr`
  hundreds of thousands of times a second, which is gigabytes of log a minute.
  `KF2_LOG=sdk` is the same order once the frame loop is running — prefer the
  patches' own probes, which report a summary per window instead of a line per
  call.
- **`KF2_CDTRACE=1`** puts a stack trace on the first CD register access
  (`patches/recompone/0002`).
- **`KF2_AUTOPAD=8:Start:400,20:Circle:200`** replays scripted input, for
  reproducing an input-triggered bug with nobody at the keyboard. Its clock
  starts when the first area module loads, which is the only point in the boot
  sequence that reliably means "in game".
- **`KF2_SHELL=1`** opens a command channel for agents on TCP 127.0.0.1:27900 —
  `state | load <slot> | warp <area> | press <button> [ms] | kill`, one request
  per line, one single-line JSON response back. See "The command channel" in
  [PATCHES_AND_MODS.md](PATCHES_AND_MODS.md).
- **`mcp/` is the same channel as an MCP server.** `KingsField2Mcp.csproj` is a
  standalone stdio server exposing those six verbs as typed tools to any MCP
  host (`KF2_MCP_ENDPOINT` overrides the endpoint). See "The MCP layer" in
  [PATCHES_AND_MODS.md](PATCHES_AND_MODS.md).
- **The attract demo is a free live session.** Leave the port at the title and it
  walks itself into an area about a minute later, with a character, an HP bar and
  eventually a death — which is how in-game behaviour gets tested without a human
  driving the menus. `AutoReload.Simulate()` kills on demand from there, and the
  death clock at `0x8019951A` can be pinned to hold any frame of the death
  sequence still. See "Auto reload" in
  [PATCHES_AND_MODS.md](PATCHES_AND_MODS.md).

### The GL backend reported nothing

Nothing in the GL backend read an error back — no `glGetError`, no
`CheckFramebufferStatus`, no `KHR_debug` callback — so a driver that refuses a
framebuffer or a call leaves that pass empty, and the first anyone hears of it is a
picture on somebody else's machine. **`KF2_GLDEBUG=1`** (`0065`) asks for a debug
context and prints every error, warning and performance message the driver sends,
on stderr as `[GL] <severity> <type> <source> #<id>: <text>`; a message repeated
every frame prints three times and then at 10, 100, 1000 with its count. `=2` adds
notifications and the managed stack of each first report, which names the call
site, since the callback is synchronous. A context without `KHR_debug` (below 4.3
and without the extension) polls `glGetError` once a present instead, which names
the error but not the call. Off, nothing is installed. This is the first thing to
ask for with a report from another GPU.

Its first run found a real one: `GL_INVALID_OPERATION in glUniform1("uScale"@18 is
int, not float)`, which was `0055`'s cached upload (see "Water on screen cost 5 ms
a frame"). After the fix, `=1` and `=2` report nothing at all over an autostart
into area 1 on radeonsi.

### For a hang, take the managed stack of the live process

A recompiled function keeps its
MIPS address in its name, so a stack trace of the hung game names the MIPS routine
it is spinning in — no logging, no rebuild, no reproduction in a debugger:

```bash
dotnet tool install -g dotnet-stack        # once
~/.dotnet/tools/dotnet-stack report -p $(pgrep -f net10.0/KingsField2)
```

This is the fastest tool in the box for a silent hang. It is what identified
the menu deadlock (see [RUNTIME.md](RUNTIME.md)) in one shot, after a static hunt had gone nowhere:
`func_80022EFC → func_8005F564 → func_8005FE64 → BiosB.PadRead` is the whole
diagnosis, read off the top four frames. Note the process must be started from
the same shell environment you run `dotnet-stack` in, or the diagnostic socket
in `TMPDIR` will not be found.

**`KF2_TRACECALL` does not exist in the tree.** It was an ad-hoc local edit to the
dispatcher, is *not* in any committed patch, and is mentioned here and in
`CLAUDE.md` only so that a reference to it is not mistaken for a missing feature.
Re-add it by hand if you need indirect-call tracing.

## Compile a mod without launching the game

Roslyn compiles mods at load, so a
typo costs a whole boot to find. A throwaway csproj that compiles the mod source
against `bin/Release/net10.0/RecompOne.Runtime.dll` and `ImGui.NET.dll`, with
`ImplicitUsings` disabled to match `ModCompiler`, catches it in three seconds.
(Learned on the widescreen mod, and moot for that one now that it is a patch and
the project build type-checks it — but it still applies to everything under
`mods/`.)

## Getting pixels out without a screenshot

The window cannot be captured from a headless shell, but the frame buffer can:
`GpuHle.Backend.ReadVram` reads the back buffer straight out of the hardware
backend from a `DrawOTag` post-hook, and `Assets.PngWriter.WriteRgba` writes it
out. The rectangle to read is the clip rect from the last `PutDrawEnv` — which is
also how you can see the game alternating buffers, `(0,0)` and `(0,240)`.

**Getting a like-for-like pair is the hard part, and two obvious ways don't work.**
Consecutive frames of one run are different views — the attract demo is walking —
and it also leaves open which frame a given draw env applies to. The same frame
*number* in two runs is not the same view either: the demo drives itself into
`fdat02` every time but not on the same schedule, so frame 175 was a wall in one
run and a staircase in the next. Both comparisons still showed the effect
(12% of adjacent pixels one 5-bit step apart against 49%), but scene and setting
were confounded.

What works is drawing **one ordering table twice**: replace `DrawOTag`, write an
E1 built from GPUSTAT with bit 9 clear, call the original, dump; then write the
same E1 with bit 9 set, call the original again, dump. Identical geometry,
lighting and textures, one bit apart. The only caveat is that the second pass
draws over the first, so semi-transparent primitives blend twice; opaque geometry
simply overwrites.

That pair, in `fdat02`:

| | dither off | dither on |
| --- | --- | --- |
| adjacent pixels one 5-bit step apart | 11.7% | 41.6% |
| mean step between adjacent pixels | 0.264 | 0.608 |
| PNG of that frame | 19.7 KB | 30.2 KB |

Two fifths of all neighbouring pixels landing exactly one quantization step apart
is the dither pattern stated numerically, and the same picture compressing 53%
larger is the same fact seen by a compressor. Zoomed, the pair is unambiguous:
identical rock and identical lighting, one smooth and one carrying the crosshatch.
`mods/dithershot` was the throwaway that produced these; it is not kept.

That "one ordering table, twice" trick is the port's standard instrument for a
picture change: it is what licensed perspective correction's 76.6% and what the
dither pair above measures. It cannot be used for the Z-buffer — the second pass
would fail every depth test against the first — see "Z-buffer" in
[RENDERING.md](RENDERING.md).

**Running headless has its own trap.** With the desktop session locked, KWin
stops sending frame callbacks and the port blocks in `SwapBuffers` forever with
`VSync=True` — 0% CPU, no log output, and it looks exactly like a hang. Set
`VSync=False` in `interface.ini` to run it without a visible window, and read the
caveat about the widened render target under "Widescreen" in
[WIDESCREEN.md](WIDESCREEN.md) before trusting a headless picture measurement.

## Profiling a frame

`patches/FrameProfiler.cs`, `patches/ProfilerPanel.cs` and the runtime's
`Diagnostics/Profiler.cs` (`patches/recompone/0045`) say where a frame's time
went, by section, on the game thread. **Shift+P** opens the panel, and recording runs
while it is open; `KF2_PROFILE=1` records from boot and prints a summary every
five seconds, and `KF2_PROFILE_OUT=profile.csv` writes every frame for
`scripts/profile_report.py`:

```bash
KF2_AUTOSTART=2 KF2_FPS=144 KF2_PROFILE=1 KF2_PROFILE_OUT=profile.csv \
    dotnet run --project KingsField2Recomp.csproj -c Release -- disc/KingsField2.cue
python3 scripts/profile_report.py profile.csv --skip 30      # drop boot and first-hit JIT
```

**What is a section without asking.** Every function `HookManager` has detoured is
timed inside `Invoke`: the recompiled body as `func_XXXXXXXX@overlay` and every
pre, post and replace delegate on its own, as `pre NoDither.BeforeDrawOTag` and so
on. With the port's patches installed that already covers the gated stages, stage
13, DrawOTag and VSync. The runtime adds `LibEtc.VSync`, `Runtime.PresentFrame`,
the window's event pump, the picture compose (`GlCore.PresentDisplay`), the
interface, `GlCore.Flush` and `LibGpu.DrawOTag`'s packet walk. Whatever nothing
claims is **game code (no section)**. Known addresses carry a label — `stage 13:
renderer (func_800342D8@game)` — from the stage table in `GAME_INTERNALS.md`.

**Self and inclusive.** Self is what a section did itself, excluding the sections
it called, so a frame's self times sum to its length (measured: 9,277 frames,
largest disagreement 0.0015 ms, which is the CSV's rounding). Inclusive adds the
children. Stage 13's inclusive time is nearly the whole frame, because DrawOTag,
the frame cap and the present all happen inside it; its self time is the renderer.

**Work, swap and wait are three different things.** Each section is in a group —
game, hook, runtime, **swap** (the thread blocked on the driver in
`SwapBuffers`) or **wait** (a sleep to a deadline: `FramePacing.Floor`,
`MenuPacing`, `LoadPacing`, `FrameClock.Throttle`, `WaitVBlanks`). A frame capped
at 144 fps is 6.94 ms whatever it did, so the figure to chase is **work**, the frame
less its waits and its swap. The panel hides the waits unless *Show waits* is on.

**The frame boundary is the end of `Runtime.PresentFrame`**, not a hook, so the
profiler's frames do not depend on the hooks it is measuring. A section still open
there — a hooked stage running a modal loop that presents its own frames, as the
in-game menu does inside stage 3 — is split: the time so far goes to the frame that
ended, and the rest to the next.

**To see inside the game's own time, time more functions.** An empty pre-hook
makes any recompiled function a section, and whatever it calls stops counting as
unattributed: *Time the 13 stages* and *Time function* in the panel, or
`KF2_PROFILE_FUNCS=stages` / `game:80040348+800342D8`. Each is a detour for the rest
of the session, which is why none is installed unasked. Drill down by timing the
callees of whatever is heaviest.

**The spikes are the other half.** Each frame also carries the GC pause time,
collections, the game thread's allocations and JIT time. The panel lists frames
over a work threshold (twice the median plus 2 ms unless set) and a click reads one
frame in the table; `KF2_PROFILE_SPIKE=12` prints them. First measurement, area 1
standing still at 144 fps with the stages timed: work 1.17 ms of 6.94 (p99 2.0),
swap 0.15 ms, and stage 13's own body the largest single cost at 0.49 ms. Every
spike past the boot was **JIT** — QuickJit is off, so first-hit code compiles fully
optimised — including a 9.0 ms stage 4 frame (8.8 ms of JIT in it) and a 5.5 ms
`HitGuard.BeforeHit` (6.2 ms).

**What it costs.** Off, one static bool test per site (3 ns per Begin/End pair,
measured). On, 47 ns per pair; a frame in an area has a median of 170 section
entries (p99 772), so 8-36 µs a frame, plus about 0.05 ms for the CSV writer, which
is itself a section (`profiler (its own reporting)`). The panel draws inside the
frame it measures, under *menu bar + panels + popups*.

**The panel was the largest section in the frame.** Area 1 at 144 fps, standing
still: the whole interface costs about 70 µs with the panel closed (menu bar 8,
dockspace 9, Output 2, popups 0, ImGui's new frame 12 and render 36), and the open
panel added 0.62 ms to that, 38% of the frame's work. Timed by part, the table was
0.467 ms, because every row was drawn, off-screen ones included, and six strings
were formatted per row per frame; the graph was 0.114 ms, re-summing every bar's
~170 sections by group. `Aggregate` was not the problem (4 µs average, 0.195 ms at
most, every 250 ms). The table is clipped to its visible rows and its strings, the
summary and the spike list are built when `Aggregate` runs; a frame's group totals
are summed once, keyed by its index. Measured after: the panel 0.135 ms (table
0.042, graph 0.051), work 1.63 → 1.12 ms against 1.02 closed, and 33 → 17 KB a
frame allocated. The numbers now refresh with `Aggregate`, four times a second.

**A swap that jumps to milliseconds is the compositor, not the port.** Native
Wayland (KWin), VSync off, a 170 Hz monitor, `KF2_FPS=1000`: the swap is 0.16-0.23 ms
and the port draws about 800 fps, but moving the window drops it into a second
state, about 200 fps with half the swaps blocking 5-10 ms (3.8 ms average, p99
frame 10.6 ms against 1.8), until it recovers. Two otherwise identical runs matched
second for second until one flipped. The driver waits for the compositor to hand
back a buffer, which it then does only at its own refresh. Nothing in the frame
changed, so a swap spike in a profile is worth checking against what the desktop
was doing before it is chased.

**What it cannot see.** Only the game thread: the SPU mixer and the CD stream
reader run on their own threads (a GC pause still stops them, and is counted). GPU
time appears only where the CPU waits for it, which is the swap; there are no GL
timer queries. The CSV is about 700 KB a second at 144 fps and several MB a second uncapped, so a few multi-minute tours at `KF2_FPS=1000` fill the `/tmp` quota — and a full quota does not only stop the CSV: MonoMod writes its detours through a temporary file, so every hook fails to install with `Disk quota exceeded` and the pacing boots broken. Delete each CSV once it is read. A run killed
from outside can leave a truncated last line, which the report script ignores.

Mechanism measured, from the console and the CSV. The panel's layout has never
been looked at by eye.

### Where stage 13's time goes

**Measure it on a tour, not on a wall.** Area 1 from slot 2 standing still is the
cheapest scene there is. A scripted run over the command channel (`warp` to each
area, 16 s standing, then eight `press Left 1400` for a full turn), `KF2_FPS=1000`,
perspective on, stage 13 and 20 of its callees timed:

| area | stage 13 incl, still (avg / p95 ms) | turning |
|---|---|---|
| 0 | 1.67 / 1.86 | 1.61 / 2.20 |
| 1 | 1.18 / 1.32 | 1.59 / 2.07 |
| 2 | 2.28 / 2.47 | 3.23 / 4.33 |
| 3 | 1.12 / 1.24 | 1.65 / 2.74 |
| 4 | 1.13 / 1.28 | 2.01 / 5.15 |
| 5 | 3.30 / 3.66 | 2.28 / 3.91 |
| 6 | 2.10 / 2.42 | 2.71 / 4.47 |
| 7 | 3.11 / 3.48 | 3.45 / 4.98 |

Inclusive includes `DrawOTag`, the present and the swap. What grows with the scene
is the same four sections everywhere, self ms per frame:

| | area 1 still | area 5 still | area 7 turning |
|---|---|---|---|
| `func_80030540` polygon list, models and tiles alike (13-27 calls) | 0.21 | 0.54 | 0.63 |
| `func_80031C94` map tiles, with `func_80031B1C`/`func_80031950` untimed inside | 0.07 | 0.42 | 0.61 |
| `func_8002F214` second polygon list (15-23 calls) | 0.05 | 0.77 | 0.57 |
| `LibGpu.DrawOTag` packet walk | 0.09 | 0.38 | 0.37 |
| `func_80032588` model submitter | 0.01 | 0.25 | 0.21 |
| `GlCore.Flush` (submits a frame) | 0.04 (32) | 0.08 (34) | 0.11 (91) |
| vertex attribute lookup (lookups a frame) | 0.01 (185) | 0.09 (1,622) | 0.08 (1,389) |

The fixed costs stay put: `func_8002E064` 0.055, `func_8002D3A8` 0.047,
`NoDither.BeforeDrawOTag` 0.05-0.07. Area 4 turning peaked at 130 submits a frame.
None of the following has been acted on.

- **The PGXP gates.** `0035` emits `if (Pgxp.CpuTracking)` beside every
  instruction (71,796 in `game.cs`). With PGXP off the branch is never taken but
  still costs (probably because the call on the untaken side stops the JIT reusing
  the `c.X` fields it has already loaded; not verified). Stripped from
  `generated/game.cs` for one run in area 1: stage 13's game code 0.577 -> 0.449 ms,
  `func_80030540` 0.207 -> 0.161, `func_80031C94` 0.073 -> 0.044, with the control
  sections within 1% (`DrawOTag` walk 0.090/0.089, `func_8002E064` 0.055/0.056).
  That is an upper bound for gating on a `static readonly` the JIT can fold, which
  keeps PGXP a boot-time switch; it forces a recompile. The libgte wrappers
  (`NormalColorDpq`) also call `PgxpCpu.Lwc2`/`Swc2` with no gate at all (198
  sites), which return straight away.
  Done since: the emitted test is now `PgxpGate.Cpu && Pgxp.CpuTracking`, where
  `PgxpGate.Cpu` is a `static readonly` fixed in `Kf2.Pgxp.Configure` (from
  `KF2_PGXP` and `KF2_PGXP_CPU`) before any recompiled code is compiled, and the
  Lwc2/Swc2 sites carry it too. Same area 1 run, 144 fps, old against new
  `game.cs`: work 1.05 -> 0.91 ms a frame, renderer `func_800342D8` self 0.217 ->
  0.177, `PolyAssembler.Replace` 0.170 -> 0.142, `func_8002D3A8` 0.047 -> 0.032;
  controls flat (`DrawOTag` walk 0.091/0.088). With `KF2_PGXP=1` the probe reads
  the same before and after (63.0% hit, 57,888 exact/s). PGXP's CPU tracking can
  no longer be switched on mid-session, which nothing did.
- **The three polygon routines are the scene-sized cost**, 1.2-2.0 ms together in
  the heavy areas. `func_80030540` is shared: `func_80031950` calls it for the map
  tiles and `func_80032588` for models. Their libgte calls are already inlined as
  direct `Gte` calls with constant register numbers, so what is left is the
  recompiled instruction stream itself: register fields on `CpuContext` and a
  checked `PSMemory` access per load and store. A hand-written C# replacement of
  `func_80030540` (a `Replace` hook, no recompile) is the largest single target; it
  has to keep its stores going through `PSMemory` or the vertex map binds nothing.
  Done since: "The polygon assembler in C#" in `PATCHES_AND_MODS.md` took it 9-16%,
  and found two thirds of the routine was the view-space clipper it calls; its
  transform and the clipper are C# too now, and the routine is 30-52% cheaper a call
  than the recompiled one ("All of the above, verified and measured" there).
- **The packet walk scales with the scene too**, 0.37 ms for 1,400 lookups' worth of
  polygons. `DrawOTag` also visits all 8,192 ordering-table entries: it writes two
  `GteDepth` statics and takes the vertex-map read test on every empty one.
- **The fixed costs are about 0.17 ms.** `func_8002E064` clears the table
  (`ClearOTagR`, `a1 = 0x2000`, a recompiled loop through libgpu's driver table):
  0.055 ms, which a native clear would take to almost nothing.
  `NoDither.BeforeDrawOTag` walks the whole table looking for E1 words that
  "Dithering: one flag" in `RENDERING.md` measured this game never links in:
  0.05-0.07 ms, which masking bit 9 in `Gpu.SetDrawMode` would replace, and would
  cover route 3 as well.
- **Batch submits** reach 91-130 a frame while turning; in area 1 29 of 32 were
  semi-transparency toggles (see "Watching a frame being built").

Outside stage 13, a `DOTNET_JitDisasmSummary=1` run shows five game functions
compiled **MinOpts** for size, stages 2 (`func_80037C0C`) and 3 (`func_8002A550`)
among them, and their hook copies too. They run on the 20 Hz tick, so they cost
per tick rather than per frame.

### What the map walk's time is

`func_80031C94`'s own body is 0.012-0.013 ms a frame in every scene; everything
under it is work it hands out. It walks all 576 cells and calls `func_80031B1C` on
the ~195 nonzero ones, which calls `func_80031950` once per half whose model is
`< 240`. That picks one of three assemblers by the cell's high bits:

- no `0x80` (the flood's lit cells, the far bulk) — `func_8002FECC`, which has
  **no clipper**: `func_8002E650` runs `RotTransPers` per vertex, then per face
  `NormalClip`, `NormalColorCol`, a `DpqColor` per vertex and `AddPrim`;
- `0x80` — `func_80030540` (`PolyAssembler`) directly;
- `0xC0` (the 3x3 around the eye, `CullCone`'s discs) on a model of `< 16` faces —
  `func_80030C94` first, which **splits every quad and triangle into four**
  (nine midpoint vertices, `func_80017244` copying the records), then
  `func_80030540` on the result.

Measured, `KF2_FPS=1000`, the eight callees timed, ms a frame (profiler overhead
of ~50 ns per call is inside these; B1C alone is ~10 µs of it):

| | area 1 still | area 1 turning | area 2 still | area 2 turning | area 5 still | area 5 turning |
|---|---|---|---|---|---|---|
| `func_80031C94` inclusive | 0.254 | 0.418 | 1.416 | 1.107 | 0.791 | 0.984 |
| `PolyAssembler` + `func_8002E7CC` (calls) | 0.21 (6.8) | 0.37 (11.1) | 0.67 (27.2) | 0.49 (23.0) | 0.41 (13.2) | 0.49 (16.3) |
| `func_8002FECC` + `func_8002E650` (calls) | ~0 | 0.06 (6.1) | 0.55 (73.3) | 0.42 (66.8) | 0.27 (35.1) | 0.31 (60.9) |
| `func_80030C94` subdivider (calls) | 0.060 (5.8) | 0.088 (8.7) | 0.068 (16.0) | 0.071 (13.0) | 0.052 (3.1) | 0.122 (7.9) |
| walk: C94 + B1C + 950 + `func_80014B88` self | 0.041 | 0.050 | 0.124 | 0.117 | 0.083 | 0.108 |

The `PolyAssembler` row counts the model submitter's calls too (about one a frame).
Area 7 was not measured: `warp 7` killed the player and autoreload put the run back
in area 1. Since then, `func_8002FECC` and `func_8002E650` are C#, 30-36% faster
including the transform. See "The unclipped assembler and its transform" in
`PATCHES_AND_MODS.md`.

Done since: the walk itself is C# too — `func_80031C94`, `func_80031B1C` and
`func_80031950` (`patches/TileWalk.cs`), verified against the recompiled routines
over 253,614 calls with 0 RAM, register or GTE mismatches. It bought 0.013 ms a
frame, which is the point: it was taken for the **scene** it enumerates, not for the
time, and the measured 192 flagged cells a frame is this section's "~195 nonzero"
read off the other side. See "The map tile walk in C#" in `PATCHES_AND_MODS.md`.

### What the model submitter's time is

`func_80032588` is a dispatcher: rotation from the Euler triple (`func_80014FE0`),
`ScaleMatrix`, `MulMatrix0`, the light colour and fog lerps, then the MO blender
`func_80034DA8` or a rigid pointer, a transform (`func_8002E650`, or
`func_8002E9B8` when no matrix is passed; never reached on the tour), and one of
three assemblers by its seventh stack byte: `0xFF` (and `0x80`) `func_8002F214`,
`0xFE` `func_80030540`, anything else `func_8002EAEC`. **Its own body is
0.005-0.047 ms a frame at 4-23 calls**, so a C# copy of it alone buys almost nothing.

The tour (`warp 0..7`, 10 s standing, a full turn, `KF2_FPS=1000`, the callees
timed), ms a frame, calls in brackets:

| | `80032588` incl | `8002F214` | `8002EAEC` | `80034DA8` incl | `8002E650` incl |
|---|---|---|---|---|---|
| area 3 turning | 0.40 (6) | 0.22 (19) | ~0 | 0.14 (18) | 0.09 (31) |
| area 5 still | 1.15 (12) | 0.93 (23) | ~0 (3) | 0.11 (18) | 0.20 (107) |
| area 6 turning | 0.43 (4) | 0.35 (18) | 0 | 0.06 (16) | 0.09 (47) |
| area 7 still | 0.60 (5) | 0.48 (17) | 0.01 (2) | 0.08 (18) | 0.11 (89) |
| area 7 turning | 0.74 (12) | 0.55 (18) | 0.01 (7) | 0.11 (20) | 0.12 (85) |

`func_8002F214` is 75-80% of it wherever it is heavy. With nothing drawn by the
submitter (areas 4 and 6 standing) it is 0.035 ms at 14 calls, which is the arm
(`func_80032400`), `func_800346CC` and `func_80031D5C`. The transform row includes
the map's calls. **The scene is not repeatable**: a second run standing at area 5's
entry drew 1.2 submits a frame against 12, because what is in view walks, so an
A/B has to be taken in one session.

**`func_8002F214` is `func_8002FECC`'s loop with GTE lighting and no fixed bias.**
The same mesh header (`func_8002E1BC`), vertex cache, `NormalClip` load order
(`p0, p2, p1`), bump allocator and `AddPrim`; four TMD types rather than two
(`0x24` FT3 `0x20` bytes, `0x2C` FT4 `0x28`, `0x34` GT3 `0x28`, `0x3C` GT4
`0x34`), the code byte is the mode byte itself, flat faces take `NormalColorDpq`
at the mean fog weight, gouraud faces `NormalColorDpq3` (plus one `NormalColorDpq`
for a quad's fourth) at the first vertex's, the slot is the mean otz plus `a1`,
and a mean of `<= 0` or a slot of `>= 0x2000` is dropped after allocation.
`func_8002EAEC` is the same routine with the codes forced semi-transparent
(`0x26`/`0x2E`/`0x36`/`0x3E`) and `a2 << 5` put into bits 5-6 of the word it
copies from the face's `+0x6`.
Area 7 turning with the libgte leaves timed: up to 1,950 `NormalClip` a frame
(also counting `func_8002F918` and `func_800302E8`) and 690 lit faces, all of them
this pair's, so about two thirds of what it tests is back-facing.

Done since: both are C# (see "The lit model assembler" in `PATCHES_AND_MODS.md`),
22-31% cheaper a call standing still, and the GTE fast path (`0047`) took another
17-41% off them, since the lighting was most of what was left. The morph (`func_80034DA8`, its base-mesh
copy and the `func_80034A74` delta decoder) is the next largest at 0.04-0.14 ms,
and `AnimSmoothing` hooks inside it.

Done since, again: `func_80032588` itself is C# too, with its caller
`func_800331B4` (`patches/ModelWalk.cs`), verified against the recompiled routines
over 81,106 submits with 0 RAM, register or GTE mismatches. It bought **three
microseconds a frame** — the prediction in this section's "a C# copy of it alone
buys almost nothing" was right — and was taken for the scene it enumerates. The
morph is still the next largest thing here, and it is now the largest recompiled
thing left inside a C# submit. See "The object and creature walk in C#" in
`PATCHES_AND_MODS.md`.

## The first frame of an area was the JIT

Reported from play as a **hitch when walking between two areas**, and explicitly
not when teleporting. `KF2_PROFILE_SPIKE=30` names it in one line: first entry to
`fdat02`, **frame 358, 297.87 ms of work of which 292.37 ms was the JIT, over 234
methods** — the assemblers, the model walk and stage 13, all compiling. It is not
one frame either. The same boot gives **frame 580 at 179.95 ms** (stages 4, 3 and
2, 66 methods), **frame 1362 at 102.28 ms** (stage 5, the session's first
projectile, 12 methods), and a tail of 40-50 ms frames for as long as the area
keeps reaching code the session has not run.

**`TieredCompilationQuickJit` is off and must stay off** — a tier-up recompiles a
hooked method and MonoMod's detour does not follow — so there is no cheap tier
here: every recompiled function is compiled by the *full optimizing* JIT the
first time it is called, on the thread that called it. Measured at about 1.2 ms a
method.

**The control says it is the compile and not the disc.** The same walk transition
into a module the session has already run — area 1 to area 0 with `fdat02` warm —
costs **15 ms**, a max frame of 25.93 ms and `JIT 6.06 ms/s`. An emulated CD read
comes out of the host's page cache in microseconds; a load has no seek.

**Why walking and not teleporting.** The full-load path (`func_80024154`, which a
save load and `patches/AreaWarp.cs` use) runs the loading screen that
`patches/LoadPacing.cs` paces to 60 Hz — measured `84 step(s) in 1816.3 ms over
105 blocking VSync call(s)` — so the JIT lands inside something that already looks
like a load, and the player arrives standing still. A boundary crossing is stage
2's own object state machine: an object record with state `0xE0` at `rec+0x4`
(table `0x80177714`, stride `0x44`), handler `0x80038DC8`, predicate
`func_80037810` testing the player's tile against the object's own tile plus the
extents at `rec+0x38`/`+0x39` and Y within `[-0xC80, +0x800]`, then `0x800162DC`
with the target area at `rec+0x3A`. That path reaches neither wait `LoadPacing`
hooks, so **the loading figure never steps** and the freeze is naked, mid-stride.
(Stepping a `goto` onto such an object's own position is how a session reproduces
a walk transition without navigating to a door.)

`patches/Prejit.cs` compiles it all ahead of time on a background thread instead,
armed at the first `OverlayLoadedEvent` — the first moment the dispatcher's
registry is populated, and four or five seconds before the first area, where
preparing the incoming module from *its* own load event would be racing the
frames it is meant to protect. `RuntimeHelpers.PrepareMethod` compiles a body
without calling it, and the set is already enumerable: `IOverlay.Functions` holds
a delegate per recompiled function and `Delegate.Method` is the method the JIT
would otherwise compile later.

**The area modules are not the bulk, and that is worth knowing before optimising
the wrong thing**: `fdat02` declares ten functions, `fdat14` nineteen — the nine
of them together are 0.3 s. The cost is `game` (1035 methods, 2.3 s), the port's
own patches (1933, 0.7 s) and the runtime (3111, 0.5 s), which is why the pass
warms all three; a replace hook spends its own time in the GTE fast path,
`GteVertexMap` and `GlCore`, and those compile on first call like everything
else. Ordered area modules, patches, runtime, `game`, then `open` and `end`,
because the pass is racing the player.

Measured at 144 fps, `KF2_AUTOSTART=2`, boot through `fdat02` to `fdat05` and then
a walk transition into `fdat08` (never run this session):

| | `KF2_PREJIT=0` | on |
|---|---|---|
| first frame of the first area | 313.44 ms, JIT 300.35 (234 methods) | 31.0-35.2 ms |
| the frames after it | 185.56, 109.21, 45.79, 44.73 ms | none over 30 |
| walk into a cold `fdat08` | max frame 26.12 ms, JIT 7.51 ms/s | max frame 8.14 ms, JIT 1.83 ms/s |
| settled | JIT 0.61-34.86 ms/s | JIT 0.12-0.36 ms/s |
| boot to in-area | 15.96 s | 12.25-12.27 s |
| peak RSS | 429.9 MB | 461.4 MB |

Boot is *faster* with the pass in, because the thread compiles ahead of the game
thread's need rather than against it. The warm-up itself is 7201 methods in about
5.0 s of background CPU at `ThreadPriority.Lowest`, and it costs 31 MB of code.

**What is left is all boot.** Eight to thirteen spikes remain in a run and every
one of them is a frame before the pass has reached that code — `open` and the
early `game` paths, under the intro and the title, which already look like
loading. One more survives at the first area and is **not** the JIT: a ~110 ms
frame of `game code (no section)` with `JIT 0` inside the autostart's own load
(`frame 343, 104.41 ms` with the pass off, `frame 362, 111.59 ms` with it on).
That is the game's blocking load path and it is the same either way. The
acceptance run is unchanged either way: 144.0 fps drawn at 20.0 ticks/s,
`[present] wide 288, plain 0, vram fallback 0`, the vertex map reading the same
hit rate, every hook attached and `boundary 3/3 DrawOTag + 3/3 VSync`.

**A method `HookManager` has already committed is left alone** (186 of them),
because MonoMod prepares its target when it installs the detour: there is nothing
to gain, and one less thing to be doing concurrently with a detour being written.
A method that will not prepare is counted and skipped — it is a method that
compiles on its first call, which is where it started.

## Watching a frame being built

`patches/FrameCapture.cs`, `patches/FrameViewerPanel.cs` and the runtime's
`Hle/GpuTrace.cs` (`patches/recompone/0046`) capture **one run of stage 13** whole
and replay it a GP0 command at a time. The profiler says which section a frame's
time went to; this says which *primitive*, which routine built it, and what it cost
downstream. **Shift+F** opens the panel and *Capture next frame* arms it;
`KF2_FRAMEVIEW_CAPTURE=20,40` captures at those seconds after boot and prints the
summary, and `KF2_FRAMEVIEW_OUT=dir` writes each capture as three CSVs — commands,
calls and runtime sections — once the GPU has answered. The commands CSV ends in
`verts`, each primitive's screen vertices with the draw offset applied, which is
what matches a row to something in a screenshot:

```bash
KF2_AUTOSTART=2 KF2_FPS=144 KF2_FRAMEVIEW_CAPTURE=25,35 KF2_FRAMEVIEW_OUT=captures \
    dotnet run --project KingsField2Recomp.csproj -c Release -- disc/KingsField2.cue
```

**What is recorded.** From stage 13's pre-hook to its post: every word written to
GP0 with the guest address it was read from, every GP1 write, the end of every
command, and every GL batch submit with its reason. The reason is the useful
part: `GlCore.Begin` used to submit on *any* mismatch, and now says which state
differed first — semi-transparency, blend, clip rectangle, texture window, mask,
depth mode, replacement texture — or that it was a fill, an upload, a copy,
texture feedback, a full buffer or the present. It also records enter and leave
for DrawCensus's twenty-two routines plus `DrawOTag` and the `VSync` thunk, with
the primitive arena's bump pointer at each. Anything else in GAME.EXE can be
added from the panel (*Add routine*, 32 at most). The hooks go in at the first
capture and stay for the session. That capture waits one frame so they compile
outside it, and the per-word path is compiled up front for the same reason.

**The starting picture is read back, not assumed.** The software VRAM holds every
texture but not the frame when GL draws it, so capture begins with a readback of
the whole of VRAM from `GlCore` (before the clock starts, and without offering it
to `0039`'s restore copies). *Checker under the frame* replaces the view with a
checkerboard instead, which is what shows a pixel this frame never covers.

**Who built a primitive is where its packet lives.** Each call's arena bump is a
byte range, and a packet belongs to the smallest range holding its header — the
deeper call on a tie, since a callee that drew everything its caller did is the one
that drew it. A command with no address (`PutDrawEnv`'s writes and its background
rectangle) or outside every range is credited *by time*, to the innermost call
running when it was sent, and says so.

**The replay is a second `Gpu` with `Detached` set.** It rasterizes in software into
its own VRAM and touches nothing global: no backend, trace, prim event, vertex map,
Z-buffer, PGXP, texture tracker or `NotifyDisplay`. So the picture is the console's,
at 1x, with none of the port's enhancements and no widescreen margin. Its counters
give each command's **fragments** (every pixel rasterized inside the clip,
transparent texels included, since the fragment shader runs for those too), a
per-pixel overdraw count and the last command to write each pixel. Fragments are
1x: at render scale N the GPU rasterizes N² of them. The view modes are the picture,
overdraw, the GL batch that last wrote each pixel, and the routine that did. A
selected routine dims everything else, outlines its primitives, and filters the
command list. Hovering a pixel names its last writer, and a click scrubs to it.

**Reading the times.** A command's *send* cost runs from its first word to its end:
decode, vertex-map lookups, batching, and any submit it forced. The OT walk between
packets is not in it. Routine times are hook to hook, so they include every hook on
that routine, this tool's included. **`DrawOTag`'s inclusive time is mostly
`FramePacing`'s frame cap**, which sleeps in a post on it; the row is named for
that.

### The port's own work in a capture

**None of the port's rendering work is a GP0 command, so the first version of the
viewer could not see any of it.** Perspective, sub-pixel and depth recovery run in
the game's own `lw`/`sw` (the vertex map) and so hide inside every routine's self
time; the AO pass and the composite run at present; and the shaders' cost is GPU
time, which no CPU timestamp measures — it surfaces later, in the buffer swap. Four
things now put it in the capture:

- **Runtime sections.** While capturing, `Profiler.Trace` records every profiler
  section's enter and leave whether or not the profiler is on: each hook's own
  delegate, the recompiled body of every hooked function, `VSync` and the present it
  makes, each `GlCore.Flush`, the AO pass, the composite, target writebacks, and the
  vertex attribute lookup in `DrawPolygon`. They are a second lane under the
  timeline (green runtime, amber hooks, red driver, grey waits) and a table with a
  GPU column. A lookup is charged to the command it ran inside.
- **GPU time.** A `GL_TIME_ELAPSED` query goes round each batch submit, the AO pass
  and the composite, and is read back frames later. A batch's time is spread over
  its primitives by 1x fragments, which is an estimate, and totalled per routine;
  the *GPU cost* view paints each pixel by its last primitive's share per fragment.
- **This frame's present.** The present inside stage 13 shows the *previous*
  frame's picture (`VSync` comes before `DrawOTag`). After the frame the capture
  keeps a tail on the trace until the next present, which is where this frame's
  last batch is submitted and its AO pass runs; the summary's GPU line is that one.
- **Vertex map work.** `GteVertexMap`'s never-reset counters are snapshot at every
  routine enter and leave: stores watched, ring scans, words bound, coordinates the
  GTE offered, loads carried on. Per command, vertices asked and recovered; the
  *vertex recovery* view paints green (all), amber (some), red (none), blue (never
  asked). The map's *time* cannot be counted per store without distorting it — the
  store path is a few nanoseconds — so it is measured as a difference: set a
  capture as the baseline, tick *Hold the vertex map off*, capture again, and read
  the Δ self columns. That held frame draws affine with no depth.

**Measured**, area 1 from slot 2, standing, 144 fps, AO on at 16 samples, render
scale 6, true colour and sub-pixel on, two captures: **the AO pass is 1.68-2.21 ms
of GPU against 0.010-0.023 ms of CPU** — the largest single cost in the frame, and
the one nothing showed before. The frame's batches were 0.42-0.55 ms of GPU (the
model submitter 0.17-0.22, the map tiles 0.09-0.12, the HUD 0.09-0.11), the
composite 0.03. The vertex map watched 23,133-23,474 stores in stage 13, ran 1,610
ring scans, bound 657 words from 255 GTE coordinates and 852 carried loads, and
recovered 402 of 638 vertices (63.0%, the probe's figure) in 0.010-0.012 ms of
lookup; the 236 misses are all the HUD's, which never passed through the GTE.
Pacing held at 144.0 fps drawn, 20.0 ticks/s, `[present] wide 288`, with and
without `KF2_PROFILE=1`. Tracing costs what the profiler costs, for the captured
frame only. Mechanism measured from the console and the CSVs; the new lane, views
and tables have not been looked at by eye, and the hold-off A/B has not been run.

**First measurement**, area 1 from slot 2, standing, 144 fps, four captures over
two boots. Stage 13 took 4.8-5.6 ms inclusive, 4.0-4.4 ms of it `DrawOTag`
with the frame cap. **196 commands** (185 polygons = 268 triangles, one sprite,
six env), sent in **0.09 ms** all told. Attribution: 185 of 185 addressed
primitives, 100%: the map tiles (`func_80031C94`) 56, the model submitter
(`func_80032588`) 63, the HUD 66. **177,149 fragments at 1x** over a 320x240 view,
every pixel covered, overdraw 2.31 average and 7 at most. No pixel is written only
once, because `present`'s background rectangle paints all 76,800 before
anything else — 43% of the frame's fragments. The map tiles are another 76,969
and the models 13,731. **31 GL batch submits, 29 of them semi-transparency toggles**
(the model submitter forced 21 and the HUD 9) and 2 depth-mode changes. **On the
frames the animated textures step** (`func_8002DC78`, a gated stage), ten uploads
bring it to 226 commands and **1.07-1.10 ms** of sending. 0.76 ms of that is a
single map-tile quad, the first primitive after them, whose submit was for a
depth-mode change.

**Closed (`0054`).** The send was not the quad. `WriteRect` blitted each upload
from 1× staging VRAM into the scaled atlas (6144×3072 at render scale 6), which
made that atlas an FBO colour attachment; the next `Flush` bound it as `uVram`
and the driver waited for the blits. The prim shader now samples a 1× texture
(`IGlVram.SampleTexture`); `LoadImage` is `TexSubImage2D` only. The first version
then `Publish`ed each draw-with-no-display-target AABB from the atlas onto that
1× texture. Uploads no longer reach the atlas, so the copy erased the texture
pages: paletted fragments discarded (index 0), objects sampled leftover
framebuffer texels. Those draws now land in 1× and `Promote` up, so the present
fallback still sees them and the uploads survive. Dest copies / `TextureBarrier`
also no longer run on batches that do not sample dest.

Stopping the scaled blit was not enough: the 1× texture was still `SampleFbo`'s
colour attachment, so the ten `TexSubImage2D`s were still render-target writes
and `GlCore.Flush` still waited — reported 0.6 ms after the atlas blit was gone.
Sample VRAM is now a texture that is never a draw attachment; GPU writes land on
a second 1× framebuffer and `CommitDraw` copies the AABB back
(`CopyImageSubData` / `CopyTexSubImage2D`). `Flush` still needs looking at in
the profiler; the picture after the split has not.

The 29 semi-transparency batch splits are a separate cost and still open.

**Water on screen cost 5 ms a frame, and it was the vertex upload (`0055`).**
`KF2_AUTOSTART=new` stands in `fdat02` facing the water. Semi-transparent water
triangles sit depth-sorted among the opaque tiles, so nearly every one splits a
batch: **687 submits a frame** against 31 in area 1. Measured at `KF2_FPS=1000`,
scale 6: work 6.4 ms and `GlCore.Flush` 3.6 ms facing the water, against 1.3 and
0.1 turned away from it (`goto` with a yaw). `FluidSmoothing` was not it: 6.42-6.46
ms on, 6.38-6.39 off, and the frame viewer's GPU time for the batches 3.78 against
3.79 ms. Timers inside `FlushCore`, per frame: `BufferSubData` **2.55 ms**, the
program and `uScale` 0.26, textures and uniforms 0.25, the draws 0.17. Each batch
rewrote offset 0 of the vertex buffer the previous batch's draw was still queued
against, and radeonsi waited — about 3.7 µs a submit. Each batch now appends at a
cursor and the buffers are orphaned when it wraps: 2.55 → 0.07 ms. `uScale` was
looked up by name every batch (and that was most of the 76 KB a frame allocated),
and the fluid uniforms were re-sent every batch; both are sent on change now.
After: `Flush` 0.50 ms, work 3.1 ms, 153 → 310 fps; fluid on 3.09-3.13 against off
3.00-3.12; area 1 at 0.67 ms, 910 fps. The depth map (`KF2_ZBUFFER_PROBE=2`) and
the occlusion readback (`KF2_AO_PROBE=2`) at that view are identical before and
after, and the run reads 144.0 fps drawn at 20.0 ticks/s, `[present] wide`. The
687 splits themselves remain. **The cached `uScale` upload was refused by the
driver for as long as it existed**: `_legacy ? (float)s : s` is a `float` in C#
whichever branch runs, and the core shader declares `int uScale`, so every update
failed and the value set at init stood. Only the dither grid reads it on the core
path; `KF2_GLDEBUG=1` named it (see "The GL backend reported nothing").

**What it costs.** Off, a null test per GP0 word and per batch submit. After a
capture, the analysis is one full replay: 5.4 ms, or 21 ms for a frame with
uploads, on the game thread straight after the captured frame. Scrubbing backwards
replays from the start, at the same cost. With the panel open after a capture and
the routines hooked: the interface section 0.185 ms against 0.116 ms without,
work 1.20 ms against 1.07, and 4-5 KB a frame allocated against 1.7. The pacing
held at 144.0 fps drawn and 20.0 ticks/s throughout, with `[present] wide 288`.

Mechanism measured, from the console and the CSVs. The panel's picture, overlays and
layout have never been looked at by eye, and whether the replayed picture matches the
game's is exactly the kind of thing only that can settle.

## What counts as verification

There are no tests. Verification is empirical, and the useful distinction — kept
deliberately all through these documents — is between **a mechanism that has been
measured** and **a picture that has been looked at**. A counter can say that 92%
of vertices recovered a depth; only a person can say whether the cave looks right.
Where a feature's mechanism is measured and its picture is not, that feature ships
switched off, and the reason is recorded with it.


## Finding the rate defects

Every rate defect in this port is one defect wearing different clothes. King's
Field has no clock: it counts time in main-loop iterations and in `VSync` calls,
and on hardware those were the same clock, because the loop ran at a whole number
of vblanks. Drawing faster prises that one number into three, and every place the
game reads one and means another is a site.

`FramePacing` handles the sites a whole-function hook can reach. The rest were
being found by noticing them in play, one at a time — which is slow, and biased
toward whatever is *annoying* rather than whatever is *wrong*. These four tools
exist to replace that.

| tool | question |
|---|---|
| `patches/RateCensus.cs` + `scripts/rate_census.py` | which words move at the render rate? |
| `scripts/find_writers.py` | which code moves them, and what should be holding it back? |
| `scripts/rate_matrix.py` | did the fix work, at every rate? |
| `scripts/check_gate.py` | does the gate still obey its own rule? |

`scripts/kf2run.py` is the shared harness (launch, drive through `KF2_SHELL`,
harvest stdout) and `scripts/callgraph.py` + `scripts/kf2model.py` are the shared
static model. Nothing here needs a recompile.

### The census: which words move at the render rate

    python3 scripts/rate_census.py --run --seconds 30
    python3 scripts/rate_census.py --compare census-20.txt census-144.txt

Runs the same scene at two render rates and ranks every word of the game's memory
by how much more often it changed at the higher one. A word on the tick clock
changes at the same rate in both runs; a word on the render clock does not.

**The sampling clock is the emulated vblank, and that is the whole trick.**
Sampling per rendered frame would move the ruler with the thing being measured;
since `0021` the vblank is a wall-clock 60 Hz grid, so both runs sample at 60/s
and are directly comparable. Two consequences follow and are worth having in mind
before reading a number:

* **The measurement ceilings at 60 changes a second.** A word stepping at 144 Hz
  reports 60, so the ratio between a 20 fps run and a 144 fps run tops out near
  **3.0**, not 7.2. Read >1.5 as render-clocked and ~1.0 as tick-clocked.
* **Below 60 fps the vblanks arrive in bursts**, several per `VSync` call, with
  nothing running between them — so a burst reads as one change, which is what
  keeps a 20 Hz stepper reporting 20 rather than 60.

The census switches `KF2_SMOOTH`, `KF2_SMOOTH_OBJECTS` and `KF2_SMOOTH_ANIM` off
for its runs, because they write interpolated values every rendered frame *on
purpose* and would otherwise be the loudest rows in the report. That is the general caveat in one
instance: **the output is a candidate list, not a defect list.** A frame counter
and a primitive-buffer cursor are legitimately per-frame too.

Records of a known structure are folded, so a field stepping in eighty object
slots is one finding rather than eighty rows. Measured standing still in area 1
at 20 against 144 fps:

    object table +0x18   4 record(s)   ratio 3.84   13.0/s -> 49.8/s
    object table +0x24   4 record(s)   ratio 4.05   13.0/s -> 52.4/s
    object table +0x40   2 record(s)   ratio 3.84   13.8/s -> 52.8/s

That is the spinning-crystal and opening-door complaint, named without anyone
knowing what a crystal is. These fields are written through an index rather than a
literal address, so `find_writers` cannot attribute them — which is itself the
expected answer for anything inside a table.

**How the attribution was got anyway, since `find_writers` could not.** The step
the tooling does not automate is reading the emitted C# for the *stage* rather
than for the address. `func_80037C0C` (stage 2) sets a base register to
`0x80177714` and advances it by `0x44` at the loop tail, so every write it makes
is `base + k` — invisible to a `lui`/`addiu` scan and obvious to a regex over one
function's body. Tallying those gave `+0x18` at 7 sites, `+0x24` at 6 and `+0x40`
at 20, which is the census's three rows and nothing else. **When `find_writers`
answers "reached through a pointer or an index", the census's own structure
folding has already told you which structure — go and read the function that walks
it.**

Two things came out of finishing it that are worth keeping:

* **The first two rows were stage 2, and it is now gated** — see "Any frame rate"
  in `docs/PATCHES_AND_MODS.md`. `+0x18` and `+0x24` fell to ratio 1.29.
* **The third row was not.** `+0x40` stayed at 3.69. Adding `800331B4` to
  `KF2_FPS_GATE` as a one-off experiment dropped it to 17.6/s, which named the
  writer as stage 13's object pass. **That is the technique worth reusing: an
  address in `KF2_FPS_GATE` is a free attribution probe.** It does not have to be
  a change you would ship — gating a drawing function ruins the picture and
  answers the question anyway, in one run.

### find_writers: which code, and at what rate

    python3 scripts/find_writers.py 8006E5CC
    python3 scripts/find_writers.py --stage 2
    python3 scripts/find_writers.py --modal
    python3 scripts/find_writers.py --audit

Four verdicts, and the fourth is the one that matters:

    tick rate: gated directly           FramePacing skips the call on a non-tick frame
    tick rate: only under gated stages  ... and everything below it
    render rate: under ungated <stage>  stage 2 and stage 13 present, so cannot be skipped
    render rate: inside modal loop      a loop that renders its own frames

**"Which stage contains this" is not enough on its own.** The in-game menu lives
inside stage 3, which *is* gated, and every counter in it still stepped per
rendered frame — because `FramePacing` decides whether the loop is *entered* and
cannot cut one in half. A modal loop is computed, not listed: a function with a
backward branch whose subtree reaches a drawing entry point. There are 54.

**The fourth verdict is no longer a defect on its own**, and that changes how to
read a report: `patches/LoopPacing.cs` paces the *frames* a modal loop produces —
at `LogicHz` if the frame drew the world, at the vblank if it did not — so a
counter stepped once per iteration of one is already on the world's clock. A
`render rate: inside modal loop` row is now a row to check with
`KF2_LOOPPACING_PROBE=1` rather than a row to fix. What is still a defect is a
counter stepped inside a *drawing function's own body*, which no whole-function
hook reaches and no deadline holds. See "Loops that render their own frames" in
[PATCHES_AND_MODS.md](PATCHES_AND_MODS.md).

**One of that class has since been fixed, and how says what to look for in the
rest.** The billboard sprites' cel index is stepped inside `func_800331B4`'s own
body, so nothing could be gated — but every slot divides one **global counter**,
`0x80195170`, and a hold/restore pair on that single word paces the whole system.
The question to ask of a render-rate row in a drawing function is therefore not
"can this be gated" but **"is there one word upstream of all of it"**. See "The
flames run at the render rate" in
[PATCHES_AND_MODS.md](PATCHES_AND_MODS.md).

`--audit` classifies every global on the per-frame path. As of writing: **547
globals, 146 held to the tick rate, 219 inside a modal loop, 182 under a stage that
presents** (read after the parser was fixed; see "The static model read nothing"
below, and the earlier count was taken against an older codegen). A writer is any function that stores to the address — an initialiser
and a reset count too — so a render-rate row is where to look, not a verdict.

Addresses are recovered from `lui`/`addiu` pairs, which is how PSY-Q reaches
statics. Anything reached through a pointer, an index or a struct base in a
register does not appear, so an empty answer means "not written through a literal
address", never "not written".

### rate_matrix: did it work

    python3 scripts/rate_matrix.py menu-scroll --fps 20 60 144
    python3 scripts/rate_matrix.py death-clock --fps 20 144 --tickrate 20 30
    python3 scripts/rate_matrix.py menu-scroll --fps 144 --env KF2_MENUPACING=0
    python3 scripts/rate_matrix.py modal-rate --fps 20 144 --env KF2_LOOPPACING=0
    python3 scripts/rate_matrix.py sprite-anim --fps 20 60 144
    python3 scripts/rate_matrix.py compass-needle --fps 20 60 144
    python3 scripts/rate_matrix.py --list

Every empirical claim in these documents should be reproducible by one of these.
The output is a markdown table ready to paste; `--json` keeps the raw numbers so a
later run can be diffed against an earlier one.

**Read the rate a scenario reports, not the duration.** Some of these numbers are
stable and some are not: the menu repeat's spin measured 1.2 ms on one run and
16.6 ms on the next at identical settings, while the repeat *rate* it produced was
37.5 and 36.0. A scenario says which of its numbers is load-bearing.

Four things in `kf2run.py` are scar tissue and are why the harness is worth having
rather than re-glued each session:

* `state.inGame` goes true as the save loads, *before* the area transition
  finishes, so a press in that window is swallowed — hence the settle delay.
* Probes report per-second windows; the first is short and the last is truncated,
  so a scenario must say which windows it means.
* Never `pkill -f` on a pattern that also matches the calling shell's own command
  line. The harness kills itself and the symptom is a run that reports nothing
  rather than an error.
* These runs open a real window. They are not headless.

### check_gate: does the gate still obey its own rule

    python3 scripts/check_gate.py          # non-zero exit on a violation
    python3 scripts/check_gate.py --stages # what every stage reaches and writes

`FramePacing` states that **"can it draw" is the test each gate entry had to
pass**. That test was applied by reading the emitted C# by hand, once, and nothing
re-applied it since — so adding an address to `DefaultGate` was an assertion. This
makes it a check: the subtree must reach no `DrawOTag`, `PutDrawEnv` or
`PutDispEnv`, and anything that does must be a recorded exception with its reason.

Running it corrected a claim in these documents. Stage 3 was described as reaching
the renderer *only* as `func_8002A550 -> func_80037B5C -> func_800342D8`; it also
reaches it as `func_8002A550 -> func_80029CBC -> func_80018E80 -> func_800226A8 ->
DrawOTag`, and it reaches `VSync` outside any modal loop entirely, through the
menu blip `func_80022DC4`. The *reasoning* survives — those are extra renders
inside the stage, not the frame's own, and the main loop still runs stage 13
afterwards — but the single-path claim was wrong, which is the kind of thing a
hand-applied rule stops catching the moment it is written down.

`--stages` prints the inverse, which is the part that goes stale in prose: what
each stage reaches and how many globals it writes, so stage 2's deliberate
exclusion stays a recorded consequence rather than a remembered decision. It also
lists **four ungated stages that could be gated and are not** — `func_8002C944`,
`func_800140AC`, `func_80016FC8` and `func_80014534` all submit nothing and write
between 1 and 14 globals each. Nobody has looked at whether those globals are
per-tick state; the tool only says they are reachable and unheld.

### The static model read nothing

**For some time every tool on `scripts/callgraph.py` was answering from an empty
graph, and saying so in a way that read as a clean bill.** Measured: 709 functions
in `game`, **0 call edges and 0 global writes**. `check_gate.py` printed `0
violation(s)` because no gated stage reached anything; `find_writers.py` answered
`no function writes this through a literal address` for `0x801930EC`, which stage 1
writes every frame. Three changes to the emitted C# had each broken one regex:

- `0035` wraps every statement in a block and appends its PGXP hook, so an `addiu`
  is `{ var _v = c.A0; c.A0 = c.A0 + 0x2E18u; if (...Pgxp...) ... }` and an access
  goes through a local, `{ var _a = (c.S0 + 0x66u); ... mem.ReadU16(_a) ... }`;
- the per-overlay classes made a call `KingsField2_game.func_...(c, m)`, where the
  call regex wanted `KingsField2.func_`;
- `merge_sdk_names.py` named 997 functions after PSY-Q, which the function regex
  (`func_` only) skipped, calls into them included.

The parser now cuts the PGXP tail off each line before reading it, tracks `_a`,
matches any overlay class and any name (a PSY-Q name's address comes from its
funcmap), and records **reads** and every address a `lui`/`addiu` pair forms as
well as writes: `Graph.readers(addr)` and `Graph.touching(lo, hi)`, the second
catching a table's base that the dataflow cannot follow into an index. After: 807
functions in `game`, 1,910 call edges, 1,265 writes, 1,896 reads; `check_gate.py` now prints those
counts first and fails outright on a graph with no edges or no writes.

**The first honest `check_gate.py` run failed**, on stage 5: `func_80046A60 ->
func_80043388 -> func_8001D544 -> func_800226A8 -> DrawOTag`. `func_80043388` is a
modal loop (a backward branch; it opens the shops, the message box and stage 13
itself, an NPC's conversation), and with every modal loop blocked stage 5 reaches
nothing that draws, so it is the same exception as stages 2 and 3 and is recorded
in `KNOWN` as such. It passes again, now on a graph that has edges in it. **A tool
whose clean answer and whose broken answer print the same line needs a count of
what it read**; the counts above are the thing to check after a codegen change.
