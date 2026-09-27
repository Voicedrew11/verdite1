# The runtime: what a static recompilation loses, and the patches for it

`tools/RecompOne/` is gitignored, so **any edit made inside it is lost on a fresh
clone**. Changes to the recompiler or the runtime must be captured as a numbered
patch in `patches/recompone/`, applied in order by `scripts/setup_tools.sh` (see
"Setting up the tools" in [DEVELOPMENT.md](DEVELOPMENT.md) for why it peels the
stack newest-first before applying it oldest-first).

**One root cause is behind half of this file.** On hardware, PSY-Q reaches the
outside world through interrupts: a finished DMA raises IRQ 3, a CD sector raises
IRQ 2, the BIOS refills the pad buffer from VBlank. A recompiled build has **no
exception path**, so none of those entry points ever runs. Nothing errors — the
transfers themselves are emulated and complete fine — and only the work the game
does *inside* the handler silently disappears.

Its sibling failure is just as quiet: **anything the runtime refreshes only at
`VSync` is invisible to a game that stops calling `VSync`.** The three CD hangs,
the menu deadlock and the dead ending screen below are all that shape, in
different libraries. When something completes but produces no visible effect, or
a wait loop never satisfies, check which of the two it is before looking anywhere
else.

## DMA callbacks: the thing that was actually missing

With libcdstream mapped, the movie ran end to end and the screen stayed black.
Everything looked right: `StFreeRing`/`DrawSync`/`PutDrawEnv`/`DrawOTag` cycling
once per frame, and `KF2_LOG=mdec` showing real decodes —

```
[MDEC] decode depth=3 signed=False bit15=True mbs=300 wordsOut=38400
```

300 macroblocks, 38400 words = 320×240×2 bytes, a full frame, 312 of them. The
decoded frames were landing in RAM and never reaching VRAM.

The reason is a general problem with static recompilation, not a King's Field
one. **On hardware a finished DMA raises IRQ 3 and PSY-Q's interrupt entry reads
DICR and calls the channel's callback. A recompiled build has no exception path,
so that entry never runs and every DMA callback is dead.** Nothing errors; the
transfers themselves are emulated and complete fine. Only the work the game does
*inside* the callback silently disappears — and here that work is the whole
picture:

```
DMACallback(1, 0x800142A0)        <- MDEC-out DMA completion callback
0x800142A0:  LoadImage(rect, buf) <- uploads one 16x240 strip
             rect.x += rect.w     <- 20 strips = one 320x240 frame
             DecDCTout(buf, n)    <- decode the next strip
```

`DMACallback` is `0x8001EAF0` / `0x8005FC30` / `0x8001B0BC`, reached indirectly
through a per-channel wrapper and libapi's own driver table, so it has no direct
`jal` anywhere — `KF2_TRACECALL` on the dispatcher is what caught it. It is
identified by its DICR arithmetic: it indexes a callback table by channel,
returns the previous entry, and on install ORs `0x00800000 | (0x01010000 << ch)`
into DICR, clearing those bits again when passed null.

`patches/recompone/0004-libapi-dma-callbacks.patch` adds
`RecompOne.Runtime.Sdk.LibApi`, which records the table and runs the callback
from `Dma.Complete`. It is deliberately **not** gated on DICR: the routine that
would have set those bits is the one being replaced, and a registered callback
already encodes the same intent.

Two things to know if this misbehaves later. The transfer completes inside the
store that starts it, so the callback runs re-entrantly on top of the caller —
`LibApi.Complete` snapshots and restores the CPU context the way
`Interrupts.Deliver` does, and the strip loop above therefore recurses about 20
deep per frame instead of iterating. And `Interrupts.Deliver` now reports a
dropped IRQ once per line; a run where *every* IRQ misses means the
interrupt-environment offset `BiosB` derives from `HookEntryInt` (`A0 - 0x36`)
does not fit this game's PSY-Q version. For KF2 the real interrupt-callback
table is at `0x8003DD44 + irq*4` and the DMA callback table at
`0x8003DD74 + ch*4`, neither of which the runtime's offset finds — which is why
routing `DMACallback` to the runtime beats trying to fix the offset.

Verified by dumping the VRAM shadow mid-playback: the From Software logo and the
title sequence, in both framebuffers. (Window capture is not available in this
session — the compositor screenshots the lock screen — so the check was on VRAM
rather than on the presented frame.)

## The three ways a CD read can hang

Every one of these is the same root cause as the DMA callbacks — **a static
recompilation has no interrupt path** — and each one presents completely
differently. The game uses all three, so all three had to be fixed, and each was
hidden behind the one before it.

**1. Polled `ReadN`.** `OPEN.EXE`'s title-screen loader issues
`CdControl(CdlSetmode, 0x80)` and `CdControl(CdlReadN, &loc)`, then per sector
runs `while (CdReady(0,0) != CdlDataReady) ;` followed by
`CdGetSector(buf, 0x200)`. The raw `CdReady` spins on a global only libcd's
interrupt handler writes. Symptom: **total silence** — the log stops dead, no
VSync, nothing, because the CPU never leaves the loop.

Fixed by mapping `CdReady` and `CdGetSector` and teaching the HLE that a read
with no callback registered always has its next sector ready; `CdGetSector`
advances the drive one sector, which is what makes the loop walk the file.

**2. `CdReadSync`.** libcd's own file reader is
`CdRead(...); while (CdReadSync(1, 0) > 0) ;`, and `CdReadSync` returns a
sectors-remaining counter that only the interrupt decrements. Same silent spin.
The instructive part: `OPEN.EXE` runs this identical routine and *survived*,
because the counter happened to read 0 and the loop fell straight through.
`GAME.EXE` read it as garbage and hung. **An unmapped SDK wait can look fine
purely by accident** — do not treat "the intro works" as evidence the library is
mapped.

**3. Kernel events.** `GAME.EXE`'s loader is event-driven. Its init opens, in
`EvMdINTR` (callback) mode:

| class | spec | handler | what it does |
|---|---|---|---|
| `HwCdRom` `0xF0000003` | `EvSpCOMP` `0x20` | `0x8001794C` | job state machine; issues each `CdRead` |
| `HwCdRom` | `EvSpDR` `0x40` | `0x80017A98` | `CdGetSector(job->buf, 0x200)` per sector |
| `HwCdRom` | `EvSpERROR` `0x8000` | `0x80017B14` | |
| `RCntCNT3` `0xF2000003` | `EvSpINT` `0x02` | `0x80017850` | frame counter the CD timeout rides on |

On hardware libcd's interrupt handler turns each CD interrupt into a
`DeliverEvent`. The runtime already has the whole event manager — `OpenEvent`,
`EnableEvent`, `DeliverEventIntr` with `EvMdINTR` dispatch, used by the memory
card — but **nothing ever delivered `HwCdRom` or root-counter events**, so the
job queue never took a step.

This one does *not* present as a freeze. The main loop keeps running and the
loading screen animates happily forever, which reads like a slow load rather
than a bug. Symptom: **everything alive, nothing progressing.**

`0005` delivers them at VSync rather than inside the command, so a handler that
issues the next command does not recurse on top of the one that called it; new
events queued by a handler are drained in the same tick, up to a bound. `EvSpACK`
goes out for every command and `EvSpCOMP` only for the commands with a second
response (`Init`/`Stop`/`Pause`/`SeekL`/`SeekP`/`Standby`), plus one at the end
of an HLE `CdRead` — modelling the pause real libcd issues once the last sector
is in, which is the completion the caller actually waits for.

| function | `open` | `game` | `end` |
|---|---|---|---|
| `CdReady` | `0x8001AF5C` | `0x8004AD18` | `0x80017528` |
| `CdReadSync` | `0x8001AF7C` | `0x8004AD38` | `0x80017548` |
| `CdGetSector` | `0x8001B224` | `0x8004AFE0` | `0x800177F0` |

These are 8-instruction thunks that match in several places, so `game` and `end`
come from the delta the unambiguous rows of the same object agree on
(`+0x2FDBC` / `-0x3A34`), confirmed by the internal routine each thunk forwards
to. **Note that is not the delta in the table below**: `CdInit` sits at
`+0x315B0`, so libcd is split over more than one object and its thunk block and
`cdinit` move independently.

## The interrupt-callback table cannot be guessed

**Symptom:** `unmapped call: 0x0BFF0FFE` from `Runtime.PresentFrame` →
`Interrupts.Deliver`, arriving on the loading screen after minutes of correct
play. The address is not code; it is not even aligned.

`Interrupts.Deliver(irq)` finds the handler to call by reading
`BiosB.IntrEnvInInterruptAddr + 2 + irq*4`, where that base comes from the
argument the game passed to BIOS `B(19h) HookEntryInt`, minus `0x36`. That
argument is a **jmp_buf**, not a callback table — in `GAME.EXE` it is
`0x8007437C`, and the word above it (`jb[1]`, the interrupt stack pointer) is
`0x8007535C`, 4 KB higher. Where the callback table sits relative to that buffer
is a property of one link of one PSY-Q version, so here the derived slot,
`0x80074348`, is an ordinary game variable. For most of the run it happens to
read zero and the delivery is dropped silently; the moment the game stores
something there, the runtime calls it.

The real table is the one libapi's `InterruptCallback(irq, func)` indexes. That
function is easy to recognise and gives the address directly: it computes
`table + irq*4`, returns the previous entry, ORs `1 << irq` into `I_MASK` through
the pointer in its `.data`, and clears the slot when passed null.
`ResetCallback` next to it zeroes 11 consecutive slots — `irq` 0 to 10 — which is
what fixes the base rather than leaving it one word ambiguous.

| overlay | `InterruptCallback` | callback table |
|---|---|---|
| `open` | `0x8001E75C` | `0x8003DD48` |
| `game` | `0x8005F8CC` | `0x8006E3D4` |
| `end` | `0x8001AD28` | `0x80038D90` |

Two independent checks. **Statically**, `table + 11*4` lands exactly on the DMA
callback table the DMA interrupt dispatcher walks (`0x8003DD74` in `open`,
`0x8006E400` in `game`) — the two tables are adjacent, as the 11-slot layout
predicts. **At run time**, the slots read back as the functions they should be:
`game` slot 3 is `0x8005FAE0`, which is that same DMA dispatcher (it masks DICR
with `0x7F000000`, walks seven channels, clears each flag and calls the
channel's callback), and slot 0 is `0x8005F45C`, the vblank handler that bumps
the frame counter and runs the registered `VSyncCallback`.

`patches/recompone/0006-irq-callback-table.patch` adds
`Interrupts.CallbackTable` for a game to set, and — for the case where nothing
has — makes the derived path refuse a handler that is not a word-aligned
function the dispatcher knows, reporting it once instead of calling it. The
addresses themselves are game knowledge, so they live in `Program.cs`, rebound
per overlay from `OverlayLoadedEvent`.

Note what this fixes beyond the crash: those two handlers had **never run**. The
vblank callback the game registers now fires once a frame, which is what
`VSyncCallback` users expect.

## The vblank fired when the game asked

**Symptom:** the title-menu music plays at roughly half speed, and locking the
frame rate below 30 (`KF2_FPS=15/20`) slows music everywhere the same way. The
title screen renders at ~15 fps, not 30.

**Mechanism:** the port advanced the emulated vblank once per HLE `VSync` call
and nowhere else. `LibEtc.VSync` incremented `_vcount`, delivered the RCNT3
vblank BIOS event (`BiosB.DeliverEventIntr`, class `0xF2000003` spec `0x0002`)
and dispatched `VSyncEvent`; `Runtime.PresentFrame` additionally ran the PSY-Q
IRQ 0 chain — all four at VSync-call frequency, and nothing else in the checkout
advanced any of them. On hardware the vblank interrupt fires every 16.7 ms
whether or not the main loop is ready: a loop blocked on a CD read misses
pictures, not vblanks. In-game the loop issues two VSync calls per rendered
frame (85.7% of frames charge two vblanks), so at 30 fps the domain ticks 60/s
and the sound sequencer — fed by root-counter interrupt events opened beside the
SPU flush (`OpenEvent(0xF2000000|n, …, EvMdINTR, …)`) — keeps time. The title
loop presents through DrawSync, one `VSync(0)`, PutDrawEnv, PutDispEnv,
DrawOTag — one VSync call per picture, and the streamed-picture rate is paced at
real drive speed by `LibCdStream.StreamLoop`, which stays. So on the title the
whole vblank domain ran at ~15 Hz and the music played half speed; at
`KF2_FPS=15` it ran at half that again.

**Fix:** `patches/recompone/0021-vblank-wall-clock.patch` advances the vblank on
a wall-clock 60 Hz grid. Each `VSync(mode >= 0)` call catches up every boundary
missed since the last one — counter increment, RCNT3 event, `VSyncEvent`, IRQ 0
— as a burst, which is what the hardware's interrupt effectively did across the
same gap. IRQ 0 moved with it, out of `PresentFrame`. Presentation cadence
(`PresentFrame`, `FrameClock.Throttle`, the FramePacing floor) is untouched. A
host stall longer than 120 vblanks (~2 s — window drag, breakpoint) discards the
stale backlog and processes only the current vblank, rather than fast-forwarding
the game through the gap. Mode semantics are
unchanged (`<0` returns the count, `==1` returns immediately); the game passes 0
at every call site in all three overlays.

**Measurement:** with a frame-stats mod (since removed), title windows go from
`vblanks/frame 1:100%` to a spread worth ~60 vblanks/s; in-area windows stay
`2:100%` at 30.0 fps. Expected behaviour change: title-screen waits counted in
vblanks (the attract-demo timeout among them) now elapse at hardware's rate
rather than at picture rate.

One loose end recorded for the next audio timing mystery: the sound driver also
opens RCNT1/RCNT2 interrupt events, which the runtime never delivers. Nothing
visibly misses them today, but they are the first suspect.

### There were three fixed 60s, and only one of them is the host's

Worth keeping straight, because moving the wrong one speeds the music up:

| constant | domain | moved by |
|---|---|---|
| `LibEtc.VBlankMs` | **guest.** The emulated vblank grid: the counter, the RCNT3 event, `VSyncEvent`, IRQ 0 — and through IRQ 0 the game's own `0x801B6CA8`, the sound sequencer and every clock the game keeps in frames. | nothing. Deliberately left at 60. |
| `FrameClock.FrameMs` | **guest since `d81dec8`.** Upstream made this the emulated vblank rate: `Interrupts.VBlankCount` is `FrameClock.Count`. Moving it now moves the grid. | nothing. |
| `FrameClock.TargetFps` | **host.** How long `Runtime.PresentFrame` waits, applied per `VSync` *call*. All that is left of `patches/recompone/0025`, and a separate block at the foot of `FrameClock` with its own grid, since `FrameMs` above is no longer free to move. Exposed as `Runtime.TargetFps`, because `FrameClock` is `internal`. |
| `FramePacing.VBlankMs` | **port.** The floor's own arithmetic. | gone — the floor is expressed in frames a second now. |

Upstream throttles in `PresentLoop`, which this port never enters, so
`Runtime.PresentFrame` calls `FrameClock.Throttle()` beside upstream's
`MarkFrame()`.

`FrameClock` cannot be a frame pacer whatever its target, because it throttles per
`VSync` call and a rendered frame can carry more than one — in an area it carries
two, one to present and one inside the game's own frame gate. `patches/FramePacing.cs`
therefore keeps its own deadline at `DrawOTag`, which is the only point that knows
where a frame ends, and hands `FrameClock` a permissive ceiling instead. See "Any
frame rate" in [PATCHES_AND_MODS.md](PATCHES_AND_MODS.md).

**The ceiling did once end up pacing the port, and it is worth knowing how.** The
port's floor only ran on frames it recognised as frames, and it recognised them by
the *guest* vblank — the first row of that table, a fixed 60 Hz grid. Above 60 fps
most frames carry no vblank, so the floor stopped firing on them, fell behind its
own deadline, reset, and left `FrameClock`'s `2 × target` ceiling as the only limit
in the loop. The port then drew at exactly twice the requested rate and the game
ran at double speed. The fix was in the port, not here: the frame boundary is now
the `VSync` *call* rather than the vblank. But the shape of it is the standing
lesson about these three 60s — **a guest clock cannot be used to measure host
time**, and this table is where to check which one a number belongs to.

One thing `0025` does not fix, noted because it is the next question a rate above
60 raises: **the runtime never queries the monitor's refresh rate**, so with vsync
on the driver's swap blocks to a panel whose rate nobody here knows. A rate above
the panel's is capped by it, silently.


## Two vblank timelines now ship, and the port's is the default

Upstream grew its own wall-clock vblank grid after the pin this port was vendored
from (`0409bc2`). `Interrupts` owns the grid — `VBlankCount`, `TickVBlank`,
`MsToNextVBlank` — and `LibEtc.VSync` *blocks* in `WaitVBlanks` until the count
reaches its target. Since `d81dec8` the grid's counter and clock are
`Host.FrameClock.Count` and `.Now`, advanced by `FrameClock.Catch()` — so
`TickVBlank` must run on **both** timelines and only the `Raise(0)` inside it is
gated. Gating the call instead froze `VBlankCount` at 0, and `BiosB`'s
memory-card pump is keyed on that count: the card never pumped, the save never
loaded, and the run sat in `GAME.EXE` at `hp 0` with no error and no CD read. See
"The merge to `d81dec8`" in `docs/RECOMPONE_FORK.md`. That is what the hardware does, and it is also **a hard 60 Hz
ceiling on every VSync call**, which is the one thing this port cannot have:
`FramePacing` hands `FrameClock` a deliberately permissive rate and keeps its own
deadline at `DrawOTag`, and `MenuPacing`, `LoadPacing` and `SpriteAnim` are each
measured against a VSync that returns immediately.

So both timelines ship and `LibEtc.BlockingVSync` chooses, defaulting to the
port's own (`patches/recompone/0021-vblank-wall-clock`). `KF2_VSYNC=block` is the
comparison. Measured at `KF2_FPS=144`, same save, same area:

| | picture | world |
|---|---|---|
| the port's grid (default) | **144.0 fps** | 20.0 ticks/s |
| `KF2_VSYNC=block` | **60.0 fps** | 19.7-20.0 ticks/s |

Both reach the area and restore the save, so this is a rate difference and not a
correctness one — but 60 is the ceiling the whole frame-rate feature exists to
get past.

**Whichever is chosen delivers each vblank exactly once**, and that took two
gates rather than one. `LibEtc.TickVBlank` calls `Runtime.DispatchIrq(0)` on the
port's timeline; upstream raises IRQ 0 from `Interrupts.PollSlow`'s own
`TickVBlank` *and* from `Runtime.PresentFrame`. Both of those are now gated on
`BlockingVSync`, because a present is not a vblank and the pair would otherwise
deliver every vblank twice — at the render rate, which is the failure the wall
clock was introduced to fix in the first place.

## The window went black: nothing drove the present

**Symptom:** after the merge to `0409bc2` the window is entirely black, and
everything else is fine. The game boots, loads and plays underneath it —
`open → game → fdat02 → fdat05`, slot 2 restored at HP 46/86 in area 1, the agent
beacon reporting a live position, `KF2_DRAWCENSUS=1` counting 8260 bytes of
primitives a frame in the HUD, the map tiles and the geometry submit. Nothing
throws and nothing warns.

**Cause, and it is two independent breaks of the same thing.** Upstream moved
presentation off the game's thread. `Runtime.Run(boot)` now starts the emulation
on a background thread and runs `PresentLoop` on the main one, and that loop is
what calls `HostWindow.Compose` → `DoRender`. It also wraps the GL backend:
`Hle.GpuHle.Backend = new Interp.InterpBackend(_glBackend)`, which records every
primitive into a `FrameGraph` and issues no GL at all until `PresentLoop` calls
`interp.Compose(i)`.

**This port enters neither.** `Program.cs` is hand-owned and calls
`Entry.Run(memory, cue)` directly — `Runtime.Run` is called from nowhere in the
tree — because the port presents from inside the game's own `VSync`, on one
thread: `LibEtc.VSync → Runtime.PresentFrame → HostWindow.Present →
PanelManager.DrawPanels`. The merge dropped the `HostWindow.Present(Gpu)` call
out of `PresentFrame` and left the interp wrapper in. So `DoRender` never ran and
the frame graph was never replayed: two ways for the same frame to reach no
screen, either of which is a black window on its own.

**The fix keeps the port's model, which is the rule the merge was resolved
by — upstream wins on structure, the port wins on behaviour.** `PresentFrame`
calls `HostWindow.Present(Gpu)` again, where it was, and `OnLoad` uses the GL
backend unwrapped so `Interp.Backend` stays null. Frame interpolation is
deliberately not carried here, so `PresentLoop` and `HostWindow.Compose` are
simply unreached; `Interp.Interp.Backend?.Publish()` in `PresentFrame` is then a
no-op, and upstream's own frame-rate control in `DisplaySettingsSection` is inert
beside the port's Video ▸ Frame pacing page.

**Why the acceptance test did not catch it.** The test recorded for a merge is
the boot walk plus `144.0 fps drawn at 20.0 ticks/s`, and every number in it was
still true with a black window: the frame boundary is a `DrawOTag` after a
`VSync`, and both of those are the *game* calling the runtime. Nothing in that
chain touches GL. **A rate measured from inside the game says nothing about
whether a picture reached the screen** — the counter to pair with it is
`KF2_PRESENT_PROBE=1`, which reads `wide 288, plain 0, vram fallback 0` when
`PresentDisplay` is being reached and prints nothing at all when it is not.

Measured after: `KF2_FPS=144` on slot 2 in area 1, `open → game → fdat02 →
fdat05`, 144.0 fps drawn at 19.9-20.0 ticks/s, the present census reading
`wide 288, plain 0, vram fallback 0` every two seconds. Never looked at by eye
here: the picture itself, which is the user's job.

## The intro movie ran at the render rate, because its pacer never armed

**Symptom:** boot the port with a high frame rate chosen and the third and
longest of the three intro movies — the one that plays the game in — runs fast.
The two before it are correct at every rate.

**Mechanism:** an STR movie is paced by the disc and by nothing else. Sectors
arrive at `LibCd.SectorsPerSecond` (150 in double speed), a frame is several of
them, and the game's display loop `func_80013DC8` blocks in `StGetNext` until the
next one is complete — so the movie's frame rate is the delivery rate divided by
its sectors per frame, and the loop's own rate never enters into it. `StreamLoop`
models that with a wall clock: it refuses to read a frame the drive would not have
delivered yet. But it only *started* refusing once a latch was set, and the latch
was set from the enqueue path — `_primed` when `_ready.Count >= PrimeFrames`, two
frames sitting decoded in the ring at the same time. **A latch that has to be
tripped can fail to trip.** The ring is 32 slots, the game drains one frame per
iteration as soon as it is ready, and this movie's frames are 13-14 sectors each:
two of them never coexisted in the ring, so `_primed` stayed false for the whole
movie and every sector was delivered as fast as the host could read it. The
picture then advanced once per iteration of the display loop, which is once per
rendered frame. The two earlier movies are 9 sectors a frame, three fit, the latch
tripped in the first few frames, and they were paced correctly — which is why the
defect looked like it belonged to one movie rather than to the pacer.

**Fix:** `patches/recompone/0026-str-pacing-without-a-latch.patch` deletes the
latch and paces from the moment the stream starts. There is no free burst on
hardware, and the burst bought nothing here: the first frame now waits the 13
sectors it would have waited on a console, which is 87 ms.

**Measurement**, by counting `KF2_LOG=mdec` decodes per second — one decode is one
movie frame:

| | movie 1 (9 sectors/frame) | movie 2 (9) | movie 3 (13-14) |
|---|---|---|---|
| before, `KF2_FPS=60` | 15/s | 15/s | **~56/s** |
| before, `KF2_FPS=144` | 15/s | 15/s | **~93/s** |
| after, `KF2_FPS=20` / `60` / `144` | 15/s | 15/s | 10/s |

10/s is the rate the disc holds it to: 13.5 video sectors a frame against 150
sectors a second, the remainder being the interleaved XA audio. The `[STR]`
counters that showed `primed=False` for the length of that movie were a temporary
probe and are not in the patch.

**The shape to keep**, since the repo already has two of these: a rate that is
enforced only after a condition is observed will run unbounded whenever the
condition cannot occur, and it fails silently and only for some inputs. The other
half of the lesson is where the movie's rate comes from at all — the disc, not the
loop — which is the same reason `patches/LoopPacing.cs` exists for loops the disc
does *not* pace.

## The menu deadlock: input only moved when the game drew

**Symptom:** press the button that opens the in-game menu and everything stops —
last frame still on screen, no error, no log output on any channel, process
alive and burning CPU. Exactly the "total silence" signature of the polled CD
read, and just as misleading.

`dotnet-stack` named it immediately:

```
BiosB.PadRead → func_8005FE64 → func_8005F564 → func_80022EFC → func_80018E80
```

`func_8005FE64` is the `B(16h) PAD_dr` thunk and `func_8005F564` is libetc's
`PadRead(id)` — `PAD_dr`, then `~*(u_long*)0x8006EAE4`, the buffer the game
registered with `PAD_init2`. `func_80022EFC` is the caller that matters:

```c
do { } while (PadRead(1) != 0);      /* wait for every button to come up */
```

It draws nothing and it never calls `VSync`. **In the runtime, host input is
polled only inside `PresentFrame`, which only runs from `VSync`** — so the pad
word this loop reads is a snapshot frozen at the last frame drawn, taken while
the button that opened the menu was still down. The release can never arrive.
The game had walked into a wait that, in this port, nothing could ever satisfy.

On hardware the BIOS fills that buffer from its own VBlank interrupt, so
`PAD_dr` is fresh whether or not the game vsyncs — the loop is perfectly
reasonable code.

`patches/recompone/0007-pad-poll-outside-frame-loop.patch` makes `PAD_dr` pump
the host itself, rate-limited to 4 ms (a game in this loop calls it ~200,000
times a second; a VBlank is 16 ms, so 4 ms is still fresher than hardware).
`HostWindow.PumpInput` takes in events and re-polls input, and redraws at most
every 16 ms so the window stays live while the game is stuck outside its frame
loop — `Present` stamps the same clock, so a normally running game never renders
twice in a frame.

This is a general RecompOne bug, not a King's Field one: any game that waits on
the pad without vsyncing deadlocks the same way. Worth an upstream issue.

Two things to take from it. First, **a wait loop that reads state the runtime
only refreshes elsewhere is the recurring shape of this port's bugs** — the three
CD hangs were the same mistake in a different library, and `VSync`-driven
delivery is load-bearing for far more than frames. Second, the phantom is
readable: when the game hangs, the last pad state in the log is the button that
was held at the last drawn frame, which points straight at the input path.

## The ending screen

`END.EXE` is a movie player. `func_800119A4` inits the GPU, then calls
`func_80011CC4` twice: `\OP\ED0.S` (1700 frames, timer `0x06A2` = 1698) and
`\OP\ED1.S` (2673 frames, timer `0x0A6D` = 2669). The last displayed frame of
the second file is the still with "The End" and the two copyright lines. After
that it waits 150 × 6 `VSync`s, `CdControl(CdlStop)`, and falls into
`while(1);` at `0x80011A50`.

On hardware that spin is the ending: the GPU keeps scanning out the last
framebuffer and the picture stays. Here a frame reaches the window only from
`PresentFrame`, which only runs from `VSync`, so the same loop leaves the
image up but the window dead — no events, no close, 100% of a core. That is
what "it crashed on The End" was. `patches/EndingHold.cs` hooks
`func_80011CC4` and, after the second movie returns, `VSync(0)`s forever
instead of reaching the spin.

### Holding the frame is faithful, and it still reads as a crash

The spin is real rather than a mis-sweep — `0x80011A50` is `08004694 00000000`
in the disc image, `j 0x80011A50` and its delay slot — and **`END.EXE` never
writes the boot stub's next-executable byte at `0x800102F0`**, so there is no
path out of the ending at all. On hardware you leave it with the reset button.

A window has no reset button, and that is the whole difference: a still picture
that answers nothing is indistinguishable from the port having died, and it gets
reported that way. It was reported that way again, as "the game crashes after
The End", after the hold was already in. Measured on both sides of the frame
rate, booting straight into `END.EXE` with `KF2_BOOTEXE=end` and again through
`GAME.EXE`'s own hand-over at `KF2_FPS=144`: `[KF2] ending: holding the last
frame` in both, process alive, no exception, and a stack sample inside
`EndingHold.AfterMovie` → `LibEtc.VSync` → `FrameClock.Throttle`. **The port was
not crashing; it was holding, and holding is what the complaint is about.**

So any button now leaves the still for the title screen (`KF2_ENDINGEXIT=0`
keeps the pure hold). The way back is the stub's own, not a new loading path:
`SLUS_001.58` is a three-entry loader — file names at `0x80010254`
(`0` = `OPEN.EXE`, `1` = `GAME.EXE`, `2` = `END.EXE`), an index at `0x80010268`,
and a loop in `func_80010038` that `Load`s the named file, `Exec`s it *as a call*
and re-reads the index from `0x800102F0` when it returns. Writing index 0 and
re-entering `func_80010038` is exactly what `GAME.EXE`'s own quit-to-title does.
`Exec` leaves `SP` alone for this stub — its header carries a zero `s_addr` — so
re-entering from inside the ending costs a few words of stack. Measured: `overlay
end overwritten by open`, `loaded overlay: open`, the IRQ callback table rebound
to `open 0x8003DD48`.

The button has to be seen going *down*, not merely found down: the movie player
accepts a skip, so whatever ended the credits can still be held when the still
appears.

**`KF2_BOOTEXE=end|game|open` is how any of this gets tested** (`patches/BootExe.cs`)
— it writes that same index before the loader loop's first pass, so the ending is
reachable in seconds instead of by finishing the game. It writes it **once**: the
loop is re-entered whenever an executable asks for another one, and the first
version wrote the index every time, which sent the return-to-title straight back
into `END.EXE`.

Two dispatcher leftovers ride in with the executable swap and are the same
shape of bug, even though they were not what froze the window:

- `HandleRegionOverwrites` only dropped an overlay that was **fully contained**
  in the new one. `GAME.EXE` is `0x5E000` bytes at `0x80011000`, `END.EXE` is
  `0x29000` at the same base, so GAME was not contained and stayed mapped —
  every `GAME.EXE` function past `0x8003A000` remained callable. The same hole
  hits a smaller FDAT module loading over a larger one, and quit-to-title
  (`OPEN.EXE` over `GAME.EXE`). `patches/recompone/0008` unloads on any
  overlap.
- The FDAT modules sit at `0x8019F07C`, which does not overlap any executable,
  so even with that fix they would survive into `END.EXE`. Its heap starts at
  `0x80100000` and covers that RAM. `Program.cs` unloads every `fdat*` overlay
  when `open` or `end` loads.


## The patches to the checkout, one by one

Twenty-one of the twenty-six are load-bearing; `0002`, `0003` and `0015` are
diagnostics, `0013` is a settings-placement hook, and `0014b` restores four
comment lines whose presence `0015`'s context assumes. Several need **no recompile**
— they change runtime behaviour only — and that is noted where it applies.

**`patches/recompone/0001-bios-load-return-1.patch` is required to boot.** The
runtime's BIOS `Load` (A(42h)) returned the header pointer, but the real BIOS
returns 1 on success. King's Field's boot stub compares the result against 1
exactly and retries forever otherwise, so unpatched it spins in the loader
(~12,900 `Load` calls in 30 seconds) and never reaches `Exec`.

**`patches/recompone/0004-libapi-dma-callbacks.patch` is required for the intro
movie.** It adds `RecompOne.Runtime.Sdk.LibApi` so DMA-completion callbacks are
delivered at all; see "DMA callbacks" above for why nothing works without it.

**`patches/recompone/0005-libcd-interrupt-driven-reads.patch` is required to get
past the title screen.** It gives `LibCd` a polled read path and makes it deliver
CD-ROM kernel events, and gives `LibEtc.VSync` the vblank root-counter event. See
"The three ways a CD read can hang" for what each one unblocks.

**`patches/recompone/0006-irq-callback-table.patch` and
`0007-pad-poll-outside-frame-loop.patch` are required to open the menu.** The
first lets a game point the runtime at PSY-Q's real interrupt-callback table
instead of deriving one that lands in game data; the second lets `PAD_dr` poll
the host, so a game waiting on the pad without vsyncing is not waiting forever.
See "The interrupt-callback table cannot be guessed" and "The menu deadlock"
above.

**`patches/recompone/0009-perspective-correct-textures.patch` is what stops the
textures swimming.** It adds `GteDepth`, a screen-position-keyed table the GTE
fills as it projects and the GPU reads as it decodes a vertex word, and teaches
both renderers to use the depth it recovers. Nothing depends on it to run — every
vertex it misses is drawn exactly as before — but it is the largest single change
to the picture in the port. See "Perspective correction" in
[RENDERING.md](RENDERING.md).

**`patches/recompone/0010-subpixel-vertex-positions.patch` is the other half of
that same recovered number** — the fraction of a pixel the GTE truncates off a
projected vertex, which is what makes geometry twitch as it moves. It extends
`GteDepth`'s slots rather than adding a table, and it is the reason `setup_tools.sh`
peels the stack before applying it: two patches now edit the same file. Off by
default. See "Sub-pixel vertex positioning" in [RENDERING.md](RENDERING.md).

**`patches/recompone/0011-gte-depth-collisions.patch` is the rest of that table.**
Screen position is not a unique key, and dropping saturated vertices made every
large nearby polygon fall back to affine. The table now keeps several samples per
pixel, records the clamp, and picks per primitive; a leftover far Z is refused
rather than applied. See "The table is not unique" in
[RENDERING.md](RENDERING.md).

**`patches/recompone/0014-gte-zbuffer.patch` is a depth buffer from that same
number.** The GPU walks the ordering table back to front with no per-pixel test,
so two surfaces that actually interpenetrate take turns in front of each other.
The recovered SZ is interpolated per pixel and tested; a miss is painter's order,
so the HUD is untouched. Window depth is a fragment value rather than clip-space
Z, because putting SZ into `gl_Position.z` far-clipped the already-projected
triangle. Off by default. See "Z-buffer" in [RENDERING.md](RENDERING.md).

The other two are diagnostics and safe to skip: `0002-cdtrace-diagnostic.patch`
names the function behind a CD register access (`KF2_CDTRACE=1`), and
`0003-libgpu-sdk-trace.patch` gives `LibGpu` the `Log.Sdk` tracing the other SDK
libraries already had, plus a `Log.Gpu` line for every GP1 write. GP1 is
display/control only — a handful of writes per mode change — so tracing all of it
is cheap, and it is the only way to see whether the game ever enabled the display.

**`0008-unload-overlapping-overlays.patch`** is the executable-swap fix written up
under "The ending screen" above: `HandleRegionOverwrites` only dropped an overlay
*fully contained* in the new one, so `GAME.EXE` stayed mapped under the smaller
`END.EXE` at the same base. Any overlap is now an overwrite. The same hole hits a
smaller FDAT module loading over a larger one, and quit-to-title.

**`0012-exact-gte-vertex-map.patch`** replaces the screen-position key of `0009`
and `0011` with the **RAM address** the coordinate is stored at, following the
value through the game's own copies into the GP0 packet. **No recompile.** See
"Following the value through memory" in [RENDERING.md](RENDERING.md).

**`0013-settings-slot-in-section.patch`** adds `SettingsRegistry.DrawSlot(slotId)`
and one call to it in the display section, so a port option can sit *inside* a
runtime section rather than in a block underneath it. UI only — **no recompile**.
See "Patch settings" in [PATCHES_AND_MODS.md](PATCHES_AND_MODS.md).

**`0015-zbuffer-occlusion-census.patch`** is the diagnostic behind
`KF2_ZBUFFER_PROBE=2` — per-polygon bbox, depth range, table position and flags,
plus a 32×16 map read back from the depth attachment. **No recompile.** See
"Z-buffer" in [RENDERING.md](RENDERING.md).

**`0016-zbuffer-clear-at-frame-head.patch`** swaps two statements in
`GlCore.PresentDisplay` so the depth clear lands at the head of a frame rather
than the tail of the outgoing one. Real and measured, but it did **not** cure the
reported symptom. **No recompile.** See "The clear landed at the tail of the
frame" in [RENDERING.md](RENDERING.md).

**`0017-mouse-capture-and-motion.patch`** exposes the cursor: `MouseCaptured`,
`TakeMouseMotion` and `IsMouseButtonDown` on `InputManager`, forwarded from
`HostWindow`. **No recompile.** See "What the runtime had to grow" in
[INPUT.md](INPUT.md).

**`0018-imgui-fractional-framebuffer-scale.patch`** recomputes
`io.DisplayFramebufferScale` as a float in `HostWindow.OnRender`, because Silk's
divides two ints. **No recompile.** See "The interface only fits a monitor whose
scale is a whole number" below.

**`0019-popups-cannot-leave-the-window.patch`** clamps a popup's size to the
viewport and finishes what `Debug ▸ Reset view` starts, so no interface scale can
put the controls out of reach. **No recompile.** See "The scale can put the
settings out of reach" below.

**`0020-theme-apply-compounds-the-style.patch`** restores the style ImGui built
before `Theme.Apply` re-themes it, because `ScaleAllSizes` multiplies every size
field and `Apply` only resets some — leaving a `WindowMinSize` that grows on each
call and eventually floors a window past the screen, over `0019`'s clamp. **No
recompile.** See "The scale can put the settings out of reach" below.

**`0037-chd-disc-images.patch`** is upstream's `137a793` back-ported: `CueFs`
becomes `DiscFs` over an `IDiscImage` with `CueBinImage` and `ChdImage` behind it,
so one disc path serves cue/bin and CHD at recompile time and at play time both.
It also carries the commit's unrelated RAM-size change, which is where
`Runtime.RamWordMask` comes from. **Forces a recompile** — the third patch that
does, after `0004` and `0035` — because `EntryWriter` emits `DiscFs.Open` into
`generated/Entry.cs`, so an existing `generated/` will not compile against the
patched runtime. See "CHD disc images" below.

**`0026-str-pacing-without-a-latch.patch`** paces an STR stream from the moment it
starts instead of after two decoded frames happen to sit in the ring together,
which for a movie of 13-14 sectors a frame never happened — so it was delivered
unthrottled and played at the render rate. **No recompile.** See "The intro movie
ran at the render rate" above.

## The interface only fits a monitor whose scale is a whole number

Reported as "the UI does not display correctly on my 1440p monitor, but correctly
on my 1080p one". It is neither the resolution nor the monitor: it is the
**compositor's scale factor being fractional**, and the port draws its whole
interface into the bottom-left corner of the window whenever it is.

`Silk.NET.OpenGL.Extensions.ImGui` 2.22.0, `ImGuiController.SetPerFrameImGuiData`:

```csharp
io.DisplayFramebufferScale = new Vector2(_view.FramebufferSize.X / _windowWidth,
    _view.FramebufferSize.Y / _windowHeight);
```

Both sides of each division are `int`, so the ratio truncates before it is ever a
float. `RenderImDrawData` then sizes **both** its GL viewport and every scissor
rectangle from it:

```csharp
int framebufferWidth = (int) (drawDataPtr.DisplaySize.X * drawDataPtr.FramebufferScale.X);
SetupRenderState(drawDataPtr, framebufferWidth, framebufferHeight);   // -> gl.Viewport(0, 0, fbW, fbH)
```

A GL viewport is anchored bottom-left, so a viewport smaller than the framebuffer
leaves dead margins along the **top and right** and clips every panel that
crosses them. An integer scale divides exactly and nothing is lost, which is the
whole of why one monitor is fine and the other is not — 2.0 is as safe as 1.0.

Measured, with the window fullscreen on the reporter's 2560×1440 monitor, which
KDE runs at `"scale": 1.15` (`~/.config/kwinoutputconfig.json`; the 1920×1080 one
is at `1`):

```
window.Size=2226x1252  FramebufferSize=2559x1439  ratio=1.1496
io.DisplayFramebufferScale = 1x1                      <- 1.1496 truncated
drawData -> ImGui GL viewport 2226x1252  (framebuffer is 2559x1439) => SHORT BY 333x187 px
```

GLFW 3.4 honours a fractional scale for the window's framebuffer (`wp_fractional_
scale_v1`), so the framebuffer really is 1.15× the logical window; it is only the
arithmetic that loses it. The reported screenshot agrees to a few pixels: a
1467×824 client area with the interface 1276 px wide (= 1467 / 1.15), a 188 px
dead strip on the right and a 131 px band at the top, both showing the GL clear
colour `(27,27,29)` — `Theme.Background`, i.e. nothing drew there.

The fix recomputes the ratio in floating point between `Update()` and `Render()`.
That window is the right one: `Update()` has already run `NewFrame`, so the
frame's layout is fixed and still in logical units — **input is untouched**, since
`io.MousePosition` and `io.DisplaySize` are both logical and stay that way — and
`ImGui::Render()` reads `io.DisplayFramebufferScale` when it fills the draw data,
so a value set anywhere between the two is what the backend sees. It cannot be
set *before* `Update()`, which overwrites it, nor after `Render()`, which has
already copied it. With it, the viewport goes to 2559×1439 — exactly the
framebuffer.

**The chrome is separately too large, and that one is ours.**
`HostWindow.QueryDpiScale()` reads `glfwGetMonitorContentScale` of the *primary*
monitor, once, at startup, and GLFW's Wayland path returns the **integer**
`wl_output` scale — so the 1.15 monitor reports **2.0**, and `Theme.Scale =
2.0 × UiScale` with the icon font baked at 26 px applies on *both* monitors no
matter which one the window is on. The reporter had already compensated by hand
with `UiScale=0.6999999` in `interface.ini`. Two things would fix it: prefer the
framebuffer/window ratio (1.15 here) over the monitor's content scale, and
re-evaluate it per frame, rebuilding the font atlas and re-applying `Theme` when
it changes — `Theme.Apply()` sets absolute sizes before `ScaleAllSizes`, so it is
safe to call again. Both are unwritten: the numbers above are measured, but how
large the interface *should* look is a judgement by eye.

One loose end, measured and not chased at the time: a **programmatic** `IWindow.Size`
set does not raise Silk's `Resize` event under GLFW-on-Wayland, so `io.DisplaySize`
goes stale. Compositor-driven resizes do raise it — the reported screenshot has the
correct logical width. Leaving fullscreen turned out to be the same hole; see the
next section.

## Leaving fullscreen left the interface at the fullscreen size

Reported from play on Linux: after fullscreen and back, the menu bar is gone and
the picture fills the window as if the interface had been hidden. Nothing was
hidden — `HideTopBar` stays false throughout — the interface was still laid out
for the fullscreen window, and a GL viewport is anchored bottom-left, so the top
of that layout, the menu bar, lay above the window.

Silk's `ImGuiController` learns the window's size **only** from `IView.Resize`,
caching it in `_windowWidth`/`_windowHeight` and writing it to `io.DisplaySize` in
every `Update()`. GLFW 3.4 on Wayland raises no window-size callback when a window
leaves fullscreen — `glfwSetWindowMonitor(NULL, …)` sets the size itself, and the
compositor's configure then matches it, so nothing is reported — only the
framebuffer-size one. Measured on KWin, a 2560×1440 monitor at scale 1.15, with
fullscreen toggled on a timer, and the events Silk raised:

```
toggle -> fullscreen   FramebufferResize 2559x1439   Resize 2226x1252
toggle -> windowed     FramebufferResize 1472x828    (no Resize)
window.Size 1280x720   io.DisplaySize 2226x1252      <- stays there
```

Four toggles out of four, so here it is every time, not "sometimes"; how visible
it is depends on how much bigger the fullscreen layout is than the window. The
controller's integer framebuffer scale also reads `0` then (`1472 / 2226`), which
`0018` overwrites with 1.15 after `Update()` — so the stale layout was drawn at
1.15× on top, pushing even more of it off the window.

`0080` compares `io.DisplaySize` — which still holds what the controller last
applied — with `IWindow.Size` before each `Update()`, and on a difference calls the
controller's private `WindowResized` with the real size (by reflection, once; Silk
is pinned at 2.22.0 and trimming is off). It catches any missed resize, not only
this one, a frame late. Measured after, same toggles: the layout goes back to
1280×720 (work area 1280×693.76, under the menu bar) on the frame after each exit.
The logged sizes are the mechanism; that the menu bar is back on screen was not
looked at by eye here. X11 and Windows were not run — both raise the event, as far
as is known, and the check does nothing when the sizes agree.

## The scale can put the settings out of reach

The other half of the same report, and the worse half: at a large enough
interface scale the settings popup grows past the window, and the parts that left
cannot be reached at all — including the UI-scale field itself, which is the one
control that would undo it. Nothing recovers on its own, because the value is
saved to `interface.ini` and read back at the next start.

Two things have to be true at once for a window to be unrecoverable, and both
were:

```csharp
// Popups/Popup.cs
ImGui.SetNextWindowPos(viewport.GetCenter(), ImGuiCond.Always, new Vector2(0.5f, 0.5f));
ImGui.SetNextWindowSize(Size * Theme.Scale, ImGuiCond.Always);
// flags: NoTitleBar | NoResize | NoMove | NoDocking | NoSavedSettings
//      | NoScrollbar | NoScrollWithMouse
```

The **size is never capped** against anything, and `SetNextWindowPos` is what sets
ImGui's `window_pos_set_by_api` — which is precisely the flag that suppresses
ImGui's own clamp of a window into its viewport (`Begin` only calls
`ClampWindowPos` when the position was *not* set by the API). So the popup is
re-centred every frame and overflows symmetrically off all four edges, with
`NoResize`, `NoMove` and `NoScrollWithMouse` closing the three ways back.

The arithmetic, for the settings popup at its declared 780×500 logical against
the default 1280×720 window:

```
Theme.Scale = HostWindow.DpiScale * ConfigManager.View.UiScale
780 * Scale > 1280  =>  Scale > 1.64      horizontally
500 * Scale >  720  =>  Scale > 1.44      vertically      <- binds first
```

`UiScale` alone ranges 0.5–3 (`ViewConfig.cs`), so the slider reaches it on any
monitor. On the reporter's it is reached at a `UiScale` of **1**, because
`QueryDpiScale` returns 2.0 there — measured, `[Host] display scale: 2x` — for a
display KDE runs at 1.15, which is the defect the section above is about. The
compensating `UiScale=0.6999999` in that `interface.ini` puts `Theme.Scale` at
1.40: the value was found by hand and sits just under the threshold, which is why
the interface was cramped rather than gone.

**Clamping the size to the viewport is what makes this impossible rather than
unlikely.** An oversized scale should cost scrolling, not cost the controls:

```csharp
var room = viewport.WorkSize - style.WindowPadding * 2f;
var size = Vector2.Min(Size * Theme.Scale, room);
if (Size.Y <= 0f) size.Y = 0f;                 // auto-height: ImGui picks it
```

The `##body` child scrolls by default — the `NoScrollbar` flags belong to the
popup window, not to it — so a fixed-height popup that had to be cut short stays
whole for free. An **auto-height** popup (`Size.Y == 0`, which is `Popup`'s
default and what the disc picker, the mods list and the notices use) needs the
ceiling stated twice: once on the window and once as a max size constraint on the
child, because an `AutoResizeY` child sizes itself to its content and would
otherwise grow straight past the clamped window and be clipped silently. A
constraint is how ImGui caps an auto-resizing child, and at the cap it scrolls.

The **escape hatch was also half-built**. `Debug ▸ Reset view` puts a default
`ViewConfig` back, but nothing reads the scale or the colours *out of* the config
after startup: the scale lives on in `io.FontGlobalScale` and in the sizes
`ScaleAllSizes` baked into the style, and the accent and background live on in
`Theme`'s own fields. Popups take `Theme.Scale` live and shrink the same frame, so
the reset landed half-done — small windows, giant text — and the one thing a
person reaches for that item to undo was the one thing left behind. It now
re-applies both (`Theme.Apply` sets absolute sizes before `ScaleAllSizes`, so
calling it again does not compound). That item is reachable at any scale: the menu
bar is anchored at the top-left corner and is the last thing to leave the window.

The third way out is the port's, `patches/UiScale.cs`:

```
KF2_UISCALE=1     force the interface scale for this run, and save it
```

It **writes** the value as well as applying it, so one run repairs a settings file
that is already past the point of being editable in the interface, rather than
having to be kept around forever. It applies on `RuntimeReadyEvent` rather than in
`Configure`: `ConfigManager.Load` runs inside `HostWindow.Initialize`, after
`Program.cs`, so anything set earlier is read straight back off disk. By that
event the ImGui context exists and the window loop has not started, and both are
on the same thread — the game and the interface share one here. Measured:

```
$ KF2_UISCALE=1.0 dotnet run …
[Host] display scale: 2x
[KF2] ui scale: 0.85x -> 1x (saved)
```

### The clamp is not the last word on a window's size

The clamp above holds until the scale is changed a few times, and then a popup
grows off the bottom of the screen — reported as "make the UI scale smaller and
then larger". The clamp is not what fails; it is overridden.

`Theme.Apply()` is called again on every accent, background and UI-scale change,
and ends in `style.ScaleAllSizes(Scale)`, which multiplies **every** size field in
the style. The block above it resets only some of them — `WindowPadding`,
`FramePadding`, `ItemSpacing`, `IndentSpacing`, `ScrollbarSize`, `GrabMinSize`,
the roundings. Everything else it does not name is multiplied again from the value
the last call left, and compounds. Measured, replaying `Apply` at 1.4, 2, 1, 6, 2
against ImGui.NET 1.90 (Silk 2.22.0):

```
default   WindowMinSize=<32, 32>   WindowPadding=<8, 8>
Apply(1.4)  WindowMinSize=<44, 44>   WindowPadding=<16, 14>   SafeArea=<4, 4>
Apply(  2)  WindowMinSize=<88, 88>   WindowPadding=<24, 20>   SafeArea=<8, 8>
Apply(  1)  WindowMinSize=<88, 88>   WindowPadding=<12, 10>   SafeArea=<8, 8>
Apply(  6)  WindowMinSize=<528,528>  WindowPadding=<72, 60>   SafeArea=<48,48>
Apply(  2)  WindowMinSize=<1056,…>   WindowPadding=<24, 20>   SafeArea=<96,96>
```

`WindowPadding` is right at every step, which is why nothing about the *spacing*
ever looked wrong. `WindowMinSize` is not, and it is the one that matters: ImGui
floors every window that is neither a child nor `AlwaysAutoResize` at it, in
`CalcWindowSizeAfterConstraint`, and does it **after** applying a size constraint:

```cpp
if (g.NextWindowData.HasFlags & ImGuiNextWindowDataFlags_HasSizeConstraint) { ...clamp... }
// Minimum size
if (!(flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_AlwaysAutoResize)))
    new_size = ImMax(new_size, g.Style.WindowMinSize);
```

So a compounded minimum beats both `SetNextWindowSize` and
`SetNextWindowSizeConstraints` — the two things `0019` clamps a popup with.
Measured on a 1280×720 viewport, asking for the clamped 1264×704:

```
WindowMinSize=  32  -> window 1264x704      with max-constraint 1264x704
WindowMinSize=  88  -> window 1264x704      with max-constraint 1264x704
WindowMinSize= 528  -> window 1264x704      with max-constraint 1264x704
WindowMinSize=1056  -> window 1264x1056     with max-constraint 1264x1056
```

Vertically only, because 1056 is past 720 but not past 1280 — which is exactly the
shape of the report. Each scale change is one more multiplication, so it takes a
few before the minimum passes the viewport height; going *down* and back up is
simply the quickest way to make several.

The fix is `0020`: snapshot the style ImGui built, once, before the first
`ScaleAllSizes`, and restore it at the top of `Apply` before re-theming. Listing
the fields `Apply` forgets would work only until upstream adds one to
`ScaleAllSizes`. Verified idempotent — replaying 1.4, 2, 1, 6, 2, 6, 6 gives
`WindowMinSize` 44, 64, 32, 192, 64, 192, 192: a function of the scale alone, with
the themed colours surviving the restore.

This was always latent — `Apply` has never been idempotent, and changing the
*accent* compounds it just as well as changing the scale. It only became visible
when the popups stopped being unbounded, since a window that already overflowed
had nothing left to reveal.

### What is measured and what is not

The geometry: the 1.44 threshold, the scale the runtime reports, the compounding,
that a compounded `WindowMinSize` beats the clamp, that the restore is idempotent,
and that the clamp and all three escape hatches fire. **How the clamped popup
reads at a large scale has not been checked by eye**, and neither has the
underlying question of what the chrome *should* look like on this monitor, which
is the unwritten fix in the section above.

## The picture is inset inside its own panel

Reported from play as thin black bars all the way round the game even in full
screen with the menu bar hidden. Three things eat that edge and only two of them
are the same kind of thing:

- **The Output panel's own window padding**, `Theme`'s `(12, 10)` multiplied by
  `Theme.Scale`. It is the largest of the three, and on a display whose scale is
  misread (the section above) it is *doubled* along with everything else, which
  is why the band looks wider on Linux than on Windows.
- **The panel's 1 px window border**, `style.WindowBorderSize`.
- **The aspect fit.** `OutputPanel.FitAspect` scales the display's aspect into
  whatever is left and centres it, so a 4:3 picture in a 16:9 window keeps a bar
  either side. That one is the picture being correct, not chrome.

The dockspace itself is already flush — `DrawDockspace` pushes zero padding, zero
border and zero rounding around `##DockHost`, sizes it to the viewport's work
area, and passes `ImGuiDockNodeFlags` `1<<12` (the internal *no tab bar*) whenever
one panel is open, so a solo Output panel has no tab strip either. The inset was
entirely inside the panel that draws the picture.

`patches/recompone/0031-output-panel-fills-its-dock-node.patch` pushes
`WindowPadding` to zero and `WindowBorderSize` to zero **around `Begin` only**,
popping both on the line after it. `Begin` reads them when it computes the
window's inner rect, so that is the whole window they need to be in force for, and
popping immediately leaves `ToastNotifications.Draw` — which lays itself out on
`style.WindowPadding` a few lines later — reading the real theme. No other panel
sees the change. UI only, **no recompile**.

What is left after it is the letterbox, and that is arithmetic rather than a
setting: a 4:3 game on a 16:9 display has bars, and the way to fill the screen is
to give the game the display's aspect — `KF2_WIDESCREEN=16:9`, or Video ▸
Widescreen — not to stretch the picture. **Never looked at by eye**: whether the
picture now runs to the edge of the dock node, and whether a toast still sits
where it did.

## The interface's font

The interface was drawn in ProggyClean, ImGui's built-in face: a 13 px bitmap.
Three things follow from *bitmap* that no amount of styling fixes — it is pixel
art, so it carries a debug-overlay look into a settings window laid over a
commercial game; it does not scale, so every size other than 13 is a stretched
bitmap and the UI-scale slider (and the misread DPI above it) blur the text
rather than resize it; and it has no glyphs outside its own small range, so a
path or a mod name with an accent draws boxes.

Upstream fixed this in `aaf7be0` ("improved font to use noto-sans"), which our
pin `870c5ba` predates, so `patches/recompone/0033-sans-serif-interface-font.patch`
is that commit back-ported: `Icons` becomes `FontSet`, Noto Sans is embedded and
added as the base face with Font Awesome merged over it in the private-use range
the icons already used, and `HostWindow` asks for `16f * _dpiScale` where it asked
for `13f`. The four call sites are `Icons.Load`, `Icons.Or` and two `Icons.Gear` /
`Icons.Ellipsis` glyphs in `ModsPopup`; nothing in `patches/` referenced the class,
so the rename costs the port nothing.

**Upstream's second face is deliberately not carried.** `aaf7be0` also embeds
`NotoSansCJK-Regular.otf` for the Japanese and Chinese ranges — 16.5 MB, which
would sit in `RecompOne.Runtime.dll` and therefore in the AppImage, the zip and
the installer. The runtime ships three languages and every string in
`languages.json` is Latin (checked: no character above U+024F), so those ranges
would render nothing anybody can select. Cyrillic, Greek and Vietnamese *are*
kept: they are Noto Sans's own coverage, they cost atlas space rather than
megabytes, and they are what a non-Latin path or mod name falls back to instead of
boxes.

**The asset is a file in this repository, not a hunk in the patch.** A 569 KB TTF
inside a `.patch` is a base85 blob that `setup_tools.sh`'s peel loop would
reverse-check on every run; instead `patches/recompone/assets/NotoSans-Regular.ttf`
is copied into the checkout between `git clean -fd` and the apply loop, and the
patch adds only the `<EmbeddedResource>` entry naming it. A missing asset stops
the script there rather than failing the build minutes later with nothing pointing
back here. The font is SIL OFL 1.1 and its licence travels with it
(`assets/NotoSans-OFL.txt`); a release that ships the font ships that file.

`FontSet.Load` falls back to `AddFontDefault` when the resource is missing, which
is the icon font's own miss the other way up: the interface loses its face, not
its text.

**This is the one patch that wants to stop applying.** When the pin moves past
`aaf7be0` the change is upstream's own, and a `FAILED TO APPLY` on `0033` means
delete it (and the asset copy in `setup_tools.sh`) rather than rebase it — though
the CJK face comes back with it, so weigh the 16 MB then.

Measured: the resource is embedded (`NotoSans-Regular.ttf` present in the built
`RecompOne.Runtime.dll`), a run prints no `[fonts]` line, and two consecutive
`setup_tools.sh` runs both report `0033 ... applied`. **Never looked at by eye** —
whether 16 px is the right size, whether the icons still sit on the baseline after
the size change, and whether anything in the port's own panes now wraps or
overflows at the larger metrics.

## CHD disc images

The port read cue/bin and nothing else, at recompile time and at play time both,
because `CueFs` was the only filesystem the runtime had. Upstream fixed that in
`137a793` ("merge back experimental chd support, heavely based on libchd"), which
our pin `870c5ba` predates by six commits, so
`patches/recompone/0037-chd-disc-images.patch` is that commit back-ported.

**What it changes is the shape rather than the format.** `CueFs` becomes `DiscFs`
and `CueBin` becomes `CueBinImage`, both behind a new `IDiscImage` — track list,
leadout, `ReadSectorData(lba, size)` — with `ChdImage` beside it over a
from-scratch libchdr port (`Cdrom/Chd/`: the header, the hunk map, a bit reader,
Huffman, LZMA, FLAC, CD-sector ECC regeneration). `DiscImage.Open` picks between
them on the file extension and falls back to the CHD magic for a file named
neither, so **one call site serves both formats** and every layer above it — the
recompiler's `OverlayWriter` and `Parser`, the runtime's `CdController` and
`BiosA`, the launcher's `DiscCheck` and `BuildKey` — only changed a type name.

**The commit bundles a second, unrelated change and it is carried.** RAM size
becomes a `PSMemory` ctor argument, and the literal `0x1FFFFCu` at three sites
(`Dma`'s linked-list walk, `LibGpu.DrawOTag` twice) becomes `Runtime.RamWordMask`
derived from it. Nothing in this port passes a size, so the RAM is the same 2 MB
it always was and the mask is the same `0x1FFFFC`; it is carried because the rest
of the commit's context is written against it, and dropping it would mean
maintaining a divergence for no gain. That change is why `setup_tools.sh`'s own
comment cites `0x1FFFFCu became Runtime.RamWordMask` as the example of upstream
drift that breaks the stack — it was the drift, and now it is in the stack.

**Two hunks conflicted, and both are in files a port patch had already edited.**
`PSMemory`'s constructor is where `0034` initialises the PGXP shadow, and its
comment said this pin has no `ramSize` argument to size it from — true then, false
now; the `PgxpMemory.Init((uint)_ram.Length)` moved below the allocation and the
comment says why it still reads the array rather than `MemoryMap`. `DrawOTag`'s
first statement carries `0003`'s SDK trace above it and `0012`'s
`WriteGp0(value, src)` below it, so the one-line mask change had to be merged
between them rather than applied.

**Codec coverage is cdzl, cdlz, cdfl, zlib and lzma.** Not zstd: a `chdman -c cdzs`
image raises `chd codec cdzs its not supported` out of `ChdCodecSet`, which reaches
`DiscCheck.Validate` as a refusal at the picker rather than as a crash minutes
later. The image measured here is v5, `cdlz`/`cdzl`/`cdfl`, one MODE2_RAW track.

**The build key does not see the format.** `BuildKey.Compute` hashes the six files
the recompile reads, and they come out identical from either container, so the same
dump as a cue and as a CHD produces the same key and shares one build directory —
switching between them costs no rebuild. The launcher's variable is `discPath` now
rather than `cuePath`; the recompiler config's key is still `"cue"`, that being its
schema, and a `.chd` goes in the same slot.

### Measured

- **The recompile is byte-identical.** `generated/` built from the CHD and from the
  cue/bin of the same dump differ in nothing (`diff -r`, 13 files).
- **It costs about half a second, once.** Three recompiles each: 0.86-0.89 s from
  the cue, 1.35-1.39 s from the CHD, against a 12.7 s first run overall.
- **An area load costs nothing measurable.** `KF2_LOADPACING=0
  KF2_LOADPACING_PROBE=1` on the autostart load reads 84 steps in **305.8 ms over
  105 blocking VSync calls** from the cue and **305.1 ms over 105** from the CHD —
  the same figure, the same waits.
- **It boots the whole chain.** `KF2_AUTOSTART=2` from the CHD walks
  `open` → `game` → `fdat02` → `fdat05`: three EXE overlays *and* two area-module
  swaps, and a module swap is armed on a CD read hitting an absolute LBA, so that
  is the raw sector path and not only the ISO directory. It lands in slot 2, area 1,
  and holds **144.0 fps drawn at 20.0 ticks/s**.
- **The STR stream decodes.** 100 s at the title from the CHD gives 1098 `[mdec]`
  lines and no errors, which is `LibCdStream` reading 2352-byte sectors.
- **The shipped launcher's first run works from a CHD.** `Verdite2` against a fresh
  data directory validates it, keys it, recompiles, compiles and plays, at 144.0 fps
  / 20.0 ticks/s.
- **The key really is format-blind.** Pointing the same data directory at the
  cue/bin of that dump afterwards runs zero recompiler lines and leaves one build
  directory (`4be97cc3237e2cd1`): the cue reused the CHD's build.

Nothing here is a picture, so there is nothing new to look at by eye: a CHD is the
same disc.

## Minimising froze the game on Wayland

With VSync on, minimising the window stopped everything: no presents, no ticks, no
probe lines, until the window came back. Measured with KWin minimising and
restoring the window by script, `KF2_FPS=144 KF2_FPS_PROBE=1`, on a 120 Hz monitor
(AMD, Mesa 25.3): **no probe line for the 13 s it was minimised**, then a
second reading 7.9 fps and 1.4 ticks. The native stack of the game thread while it
was hidden is `eglSwapBuffers` → `dri2_wl_swap_buffers_with_damage` →
`wl_display_dispatch_queue`: a swap at interval 1 waits for the compositor's frame
callback, and a compositor sends none to a surface nobody can see. The port
presents from inside the game's own `VSync`, so that one wait holds the world, the
disc and the sound as well as the picture.

**Asking for interval 0 did not fix it, which is how the second defect showed.**
`ApplySwapInterval` asked for -1 (adaptive) and logged `swap interval: -1
(adaptive)`, but the swap still waited for a frame callback at 0. Silk's own
`VSync` — set from `WindowOptions` at creation and again from `SetVSync` — puts
interval 1 back over it. So the log had been wrong on every platform: the game ran
at interval 1, never adaptive. (Measured on the old code: 119.9 fps drawn at
`KF2_FPS=144` with VSync on, which is what interval 1 on a 120 Hz monitor looks
like, and not the "-1 clamps to 0, so no vsync" an outside review predicted for
EGL.) With Silk's VSync held false, interval 0 on Wayland does not wait.

`0064` makes `ApplySwapInterval` the only thing that sets the interval. Off
Wayland, VSync on is interval 1, which is what had been running all along; adaptive
was never actually shipped, and it tears on a late frame, so it was not brought
in. On Wayland the swap stays at 0 — a Wayland compositor never tears, so the swap
was only ever adding the wait — and `FrameClock.WaitRefresh` holds one present per
monitor refresh on the CPU, on its own grid, before the swap. Measured after, same
setup: **120.0 fps drawn and 19.8-20.8 ticks a second throughout the 13 s
minimised**; 119.9-120.0 fps with the window shown; VSync off still 144.0 at
20.0 ticks, `[present] wide 288`; `swap interval: 0 (Wayland: vsync held on the
CPU at 120 hz)` in the log.

The mechanism is measured; the picture is not. A CPU grid is not locked to the
compositor's refresh the way a blocking swap is, so it can drift against it and
drop or repeat a frame at the beat — the same thing the port's own default pacing
(VSync off, a CPU deadline) already does. Whether that is visible at 120 Hz has
not been looked at. X11 and Windows are unchanged in effect (interval 1) and were
not run.

**That last sentence was wrong, and the next section is why.**

## VSync on Windows

Reported from Windows (AMD Radeon integrated graphics, 60 Hz, fullscreen): with
VSync on, "buffer swap + driver" dominated the profiler and the frame times were
awful; with it off, the picture tore. Three things stacked, and each hid the
next. Every number below is from `KF2_AUTOSTART=2 KF2_FPS_PROBE=1 KF2_PROFILE=1`
standing in area 1, fullscreen 1920x1200, the default settings otherwise.

**VSync on at boot was interval 0.** `0064` holds Silk's `VSync` false so that
`ApplySwapInterval` owns the interval, and sets 1 from `OnLoad`. But Silk applies
its `VSync` lazily, inside the first `DoRender` after the property is set, so its
`false` landed after `OnLoad` and before the first frame. Read back with
`wglGetSwapIntervalEXT`: 1 straight after the port set it, 0 at the first present
and every present after, with `swap interval: 1` in the log. A boot with VSync on
drew **112 fps on a 60 Hz monitor** — so the tearing with VSync "on" was VSync off,
and only toggling the setting in play, which calls `SwapInterval` again after the
first frame, ever produced interval 1. The same held on X11. Silk is now told the
interval the port chose (`0064`, amended), and it reads back 1.

**With interval 1 real, every frame swapped twice.** `0007`'s pad poll draws and
swaps from inside a pad read when the game seems stuck outside its frame loop,
which it judged by 16 ms since the *start* of the last `Present`. With the swap
waiting for a 60 Hz refresh, every frame is at least that long, so almost every
pad read drew a second frame and paid a second blocking swap: **21-37 fps, 22 ms a
frame in the swap**, and 16 ms of it charged to whatever read the pad (stage 3,
and "CD, card and pad ticks"). That is the report. The poll now draws only when
the game has not presented for 250 ms, measured from where `Present` ended. After:
work back to 6.5 ms a frame.

**The driver's VSync itself was unreliable, so Windows no longer uses it.** With
both fixes in, interval 1 still fell into stretches of **30-55 fps** with 17-24 ms
in the swap, in runs of 40 s alternating with good ones, and the frame intervals
spread continuously from 16 to 40 ms rather than snapping to whole refreshes. It
was first read as heat, since the bad runs followed builds; it is not — **VSync off
at 60 fps in the same minute held 60.0 at 17.9 ms p99**, with the GPU as idle
between frames as VSync would leave it. Whatever the driver does at interval 1 is
the cause, and it is not ours to fix. Its swap blocks until the flip it queues:
waiting for the vblank and *then* swapping at interval 1 locks at exactly 30.0,
one refresh in the wait and one in the swap, every run.

**So `0066` keeps the interval at 0 and waits for the blank itself** — composes
and flushes the frame, waits on the kernel's own vblank event for the monitor the
window is on (`D3DKMTWaitForVerticalBlankEvent`, `Host/Window/VBlankWait.cs`,
reopened when the window moves) and then swaps. Four runs alternated with the
interval at 60 fps: **99-100% of frame intervals within a millisecond of 16.7 ms,
p99 16.9-20.2 ms**, against 93-97% for the interval in its good stretches; at
`KF2_FPS=240`, 60.0 and 99%. The wait shows in the profiler as "VSync wait", not as
the swap. A failed wait falls back to the interval rather than running uncapped.
Minimised for 12 s, windowed, it held 59-60 fps at 20 ticks, since the monitor keeps
blanking whether the window is shown or not.

**In GLFW's fullscreen it tore, seen in play.** That mode bypasses the compositor,
and at interval 0 the flip is not held to the blank. So the wait is only taken where
the compositor presents the window — windowed, and a new **Borderless** display
mode, a borderless window the size of the monitor (Video ▸ Display mode: Windowed,
Fullscreen, Borderless; `Borderless` beside `Fullscreen` in the view config, so F11
and the menu bar still toggle "covers the screen"). Fullscreen keeps the interval.
Alternating Borderless against Fullscreen at 60 fps: Borderless **98% and 100%**
within the millisecond (the first run at 52-59 fps with its game code at 10 ms
rather than 8.6, and 33 ms frames in both); Fullscreen **0%, locked at 30.0 for
the whole run, then 96%**. Whether Borderless is really composed on this driver —
that is, whether it tears — has to be looked at; no counter here says. Game code
reads 1.5-2 ms higher in Borderless in both pairs, not chased.

**The window those numbers were measured on was not the monitor; it was the work
area.** Silk's `IMonitor.Bounds` is `glfwGetMonitorWorkarea` (the only one of the
two calls its `Windowing.Glfw` assembly references), so Borderless sized itself to
the screen *less the taskbar*: measured 1920x1128 on a 1920x1200 monitor at 150%,
per-monitor DPI aware at 144 dpi, so not a scaling fault. Reported from play as
not covering the taskbar and not filling the screen, whatever the aspect. The
rectangle is now the monitor's own, `glfwGetMonitorPos` and its current video mode,
for the GLFW monitor whose work area is Silk's. Measured after: the window at
`0,0 1920x1200`, client the same, and `SHQueryUserNotificationState` reading 3
(`QUNS_RUNNING_D3D_FULL_SCREEN`) where the work-area window left the shell
unaffected; 57.9-60.1 fps drawn at 19.7-20.7 ticks/s in area 1 with VSync on.
**And it tore, seen in play.** The mode exists to be *composed*, and a window
exactly its monitor's size is the shape Windows promotes off the compositor to a
flip of its own, not held to the blank at interval 0 — the way GLFW's fullscreen
tore; state 3 was saying so. So the window is **one row taller than the monitor**,
overhanging the bottom edge: `0,0 1920x1201`, and the shell reads 2
(`QUNS_BUSY`, a window covering the screen) rather than 3. The row lost is below
the picture's letterbox at any aspect but the monitor's own, where it is half a
game pixel or less. That the shell state tracks the promotion is inference; what
settles it is the eye, and the overhanging window was seen in play covering the
taskbar without tearing.

**Off Windows, Borderless is the window manager's fullscreen.** The monitor-sized
window never covered the screen there. A Wayland client cannot set its own
position, so it came out undecorated, fixed at 2560x1440 (`set_min_size` =
`set_max_size` on the wire) against a 1920x1034 work area, with no
`set_fullscreen` ever sent; under XWayland, KWin fitted it to the work area,
2560x1189 with the panel uncovered and no `_NET_WM_STATE_FULLSCREEN`. The mode
exists for the vblank wait, which is Windows-only, so elsewhere VSync is the
interval (or `0064`'s CPU hold on Wayland) whichever mode is chosen, and Borderless
takes GLFW's fullscreen. Measured after: `set_fullscreen(wl_output#17)` and a
fullscreen `configure` at the output's size on Wayland; `_NET_WM_STATE_FULLSCREEN`
with no size hints under XWayland. GLFW's X11 fullscreen also sets
`_NET_WM_BYPASS_COMPOSITOR`, so on X11 the two modes are the same mode. A native
X11 session has not been run.

`FramePacing`'s own 60 fps floor stays in series with the wait. Standing it down
whenever VSync already holds the frame to the refresh was tried and measured,
alternating: 92% and 99% within the millisecond without the floor, 100% and 99%
with it. Nothing to show for it, so it was not kept.

**Everywhere else VSync is the interval, with the swap deferred** — Fullscreen on
Windows, X11, and Windows when the vblank cannot be opened. The frame's GPU work
is about 11-13 ms on this machine (the frame viewer: 10.7 ms of it the map tiles
with 16x filtering, the occlusion pass 1.0-1.2 ms), which fits in 16.7 ms only when
the GPU can work on one frame while the CPU builds the next. The occlusion pass and
the composite are issued at present, and a blocking swap holds the game's thread
until the GPU has finished them and the flip has happened. So on the interval path
`0066` issues the swap at the start of the *next* present. Measured on Windows in
Fullscreen at
interval 1 with SSAO on High, to put the GPU at its limit, alternating: **60.0 fps
deferred, 56.0 immediate, in both rounds**. `KF2_SWAP=immediate` is the
comparison, and the cost is one refresh of latency. It is the path X11 takes now,
where it has not been run.

## Two general shapes worth keeping

`0007`, `0008` and `patches/EndingHold.cs` are the pattern to keep in mind:
**anything the runtime refreshes only at `VSync` is invisible to a game that stops
calling `VSync`**, and that failure mode is always silent.

`0004`, `0005` and `0006` are the other one: **a handler that never runs loses
only the work inside it**, so the symptom is never an error — it is a picture that
stays black, a job queue that never advances, or a callback table that reads as an
ordinary game variable until the game happens to store something in it.

## Upstream contribution policy

RecompOne's maintainer rejects AI-authored pull requests outright ("AI PRs will
be rejected, no exceptions"). That governs contributions to RecompOne itself; the
tool is MIT and using it here is unaffected. It does mean any fix to the
*recompiler* should go upstream as an issue rather than a PR, unless you write
the patch yourself.
