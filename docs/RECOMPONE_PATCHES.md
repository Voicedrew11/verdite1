# The port's changes to RecompOne, one by one

`patches/recompone/*.patch` are no longer replayed (the tree is vendored — see
`docs/RECOMPONE_FORK.md`), but they are still the record of what the port changed
in the runtime and the recompiler and why, and the numbering is still how each
change is referred to in the source. `docs/RUNTIME.md`'s "The patches to the
checkout, one by one" covers the early ones at more length; this list is the
complete one.

**A new number is for a new mechanism.** A correction to an existing patch's own
logic amends that patch instead: a "Since amended:" paragraph in its entry, the
amendment's diff appended to its `.patch` file, and the source comments keep its
number. `0016` and `0057` predate this and keep their numbers, since the source
refers to them.

Sixty-six of the seventy-four are load-bearing; `0002`, `0003`, `0015`, `0045`,
`0046`, `0065` and `0069` are diagnostics and `0013` is a settings-placement hook. `0063` is retired:
it was folded into `0054` as an amendment, and the number is not reused. `0074`-`0076`
are held by the remaster's plan for work not yet made (`docs/REMASTER.md`), so the
shadows are `0077`. **Three force a recompile** —
`0004`, `0035` and `0037`; every other one changes runtime behaviour only. **One
patch has an asset beside it**: `patches/recompone/assets/` holds the TTF `0033`
embeds, which is now simply a tracked file in the vendored tree.

Four files in the directory have no entry below:

- `0002-cdtrace-diagnostic.patch` — names the function behind a CD register
  access (`KF2_CDTRACE=1`).
- `0003-libgpu-sdk-trace.patch` — `Log.Sdk` tracing for `LibGpu`, plus a `Log.Gpu`
  line for every GP1 write.
- `0014b-zbuffer-depthmap-comment.patch` — restores four comment lines whose
  presence `0015`'s context assumed.
- `0021-vblank-wall-clock.patch` — advances the emulated vblank on a wall-clock
  60 Hz grid rather than when the game asks; `KF2_VSYNC=block` is upstream's
  blocking timeline. See "The vblank fired when the game asked" in
  `docs/RUNTIME.md`.
- `0045-frame-profiler.patch` — a diagnostic: `Diagnostics/Profiler.cs`, and
  sections around `HookManager.Invoke` (the hooked body and each delegate apart),
  `LibEtc.VSync`, `Runtime.PresentFrame`, the window's events, render and swap,
  `GlCore.Flush`, `LibGpu.DrawOTag` and the two host waits. The frame boundary is
  the end of `PresentFrame`. One bool per site while off. **No recompile.** See
  "Profiling a frame" in `docs/DEVELOPMENT.md`.
- `0046-frame-capture-trace.patch` — a diagnostic: `Hle/GpuTrace.cs`, an
  `IGpuTrace` sink that receives every GP0 word with its source address, every GP1
  write, the end of each command, and each `GlCore` batch submit with **why** it
  happened (`FlushReason`: target, full, texture feedback, fill, copy, upload,
  readback, present, or the first mismatched state `DesiredMatches` found). `Gpu`
  gains `Detached` — a second instance that rasterizes in software into its own
  VRAM and reaches nothing global (no backend, trace, prim event, vertex map, Z,
  PGXP, texture tracker or `NotifyDisplay`) — plus `CopyStateFrom`, the draw-area
  getters, and replay counters (`Coverage`, `Owner`, `Fragments`) that only a
  detached instance fills. `GlCore.ReadVram` gets an overload that skips `0039`'s
  snapshot, so reading the whole of VRAM back does not evict a menu's restore copy.
  One null test per word while off; `HleOn` becomes an instance property.
  **The port's own work is reported too**, because none of it is a GP0 command:
  `Profiler.Trace` receives every section's enter and leave and records them with
  the profiler off (`HookManager` takes the profiled path while it is set), and
  four sections are new — the AO pass, the composite, `Writeback` and the vertex
  attribute lookup in `DrawPolygon`. `IGpuTrace.Vertices` reports each polygon's
  lookups and hits, and `IGpuTrace.Work` a `GL_TIME_ELAPSED` query around each
  batch submit, the AO pass and the composite (`GlCore.GpuTimeNs` reads one back;
  queries exist only while a sink is set). `GteVertexMap` gains never-reset
  counters (`Stores`, `TraceScans`, `TraceBound`, `TracePublished`,
  `TraceRepublished`) off the hot path. **No recompile.** See "Watching a frame
  being built" in `docs/DEVELOPMENT.md`.

- `0001-bios-load-return-1.patch` — BIOS `Load` must return 1, not the header
  pointer. Without it the boot stub spins in the loader forever.
- `0004-libapi-dma-callbacks.patch` — adds `Sdk.LibApi` so DMA-completion
  callbacks run at all. A static recompilation has no exception path, so PSY-Q's
  IRQ-3 handler never runs and every DMA callback silently dies. Nothing errors;
  the work the game does *inside* the callback just disappears. Keep this in mind
  whenever something completes but produces no visible effect.
- `0005-libcd-interrupt-driven-reads.patch` — the polled read path and CD-ROM
  kernel events; without it the game hangs on the loading screen.
- `0006-irq-callback-table.patch` — the runtime otherwise derives the PSY-Q
  interrupt-callback table from the `HookEntryInt` argument, which for this game
  lands in game data and eventually calls a data word. `Program.cs` supplies the
  real per-overlay address; the patch also makes the derived path refuse a
  handler that is not a known function.
- `0007-pad-poll-outside-frame-loop.patch` — host input used to be polled only
  inside `PresentFrame`, so a game busy-waiting on the pad without vsyncing read
  a frozen snapshot forever. King's Field's screen transitions all begin with
  such a wait; this is what hung the in-game menu.
  Since amended: the poll also drew and swapped whenever 16 ms had passed since
  the *start* of the last `Present`, to keep the window live during such a wait.
  With the swap waiting for a 60 Hz refresh every frame is at least that long, so
  the pad read drew a second frame and paid a second blocking swap on almost every
  frame: 21-37 fps with VSync on, and 6.6 ms a frame in "CD, card and pad ticks".
  It now draws only once the game has not presented for 250 ms, measured from where
  the last `Present` ended. The amendment's hunks are in `0066`'s file, where they
  share hunks with the deferred swap. See "VSync on Windows" in `docs/RUNTIME.md`.
- `0008-unload-overlapping-overlays.patch` — `HandleRegionOverwrites` only
  dropped an overlay fully contained in the new one. `END.EXE` is smaller than
  `GAME.EXE` at the same base, so GAME's functions past `0x8003A000` stayed
  mapped after the ending loaded. Any overlap is now an overwrite.

- `0009-perspective-correct-textures.patch` — the GPU is handed polygons with no
  depth in them, so it can only interpolate U and V linearly and every texture
  swims. The depth still exists one step earlier: `Gte.Rtp` produces the screen
  position and the view depth in the same call, and that screen position is
  bit-for-bit what reaches the GP0 packet. `GteDepth` keys a small table on it, so
  the two halves are reunited without following a register or a store. A miss is
  the old affine behaviour, which is what makes it safe on by default. See
  "Perspective correction" in `docs/RENDERING.md`.

- `0010-subpixel-vertex-positions.patch` — the GTE projects to 16.16 and then keeps
  only the whole part, so a vertex drifting slowly holds still and then jumps a
  pixel and its polygon twitches. The fraction is the low sixteen bits of the same
  expression `0009` takes the depth from, so `GteDepth` carries both and serves them
  independently. The hardware backend needed nothing — its vertex position was
  always a float — and the software rasterizer now works in sixteenths of a pixel
  for any triangle that recovered a fraction, which scales its edge functions and
  its area by 256 and changes no ratio taken from them. See "Sub-pixel vertex
  positioning" in `docs/RENDERING.md`.

- `0011-gte-depth-collisions.patch` — screen position is not a unique key, and
  dropping saturated vertices made every large nearby polygon fall back to affine
  (the texture looking as if the camera jumped). The table keeps several samples
  per pixel, records the clamp for depth only, and picks per primitive; a leftover
  far Z is refused rather than applied. Positions stay on the packet — moving a
  clamped vertex to its true projection opened holes. See "The table is not unique"
  in `docs/RENDERING.md`.

- `0012-exact-gte-vertex-map.patch` — screen position was never an identity, so
  `0011`'s picking between samples was scoring a collision rather than avoiding one,
  and a wrong W throws a texture across the screen. `GteVertexMap` keys on the
  **address** the coordinate is stored at instead: a `swc2` publishes the depth and
  the fraction, a store binds them to its destination, a load of a bound address
  republishes them so they follow the game's whole-word `lw`/`sw` into the packet,
  and `DrawPolygon` asks by the address `DrawOTag` read the word from — verifying the
  word before answering. No codegen change, so **this one needs no recompile**. The
  old table stays behind `KF2_PERSPECTIVE_FALLBACK` for comparison only. See
  "Following the value through memory" in `docs/RENDERING.md`. A later edit put a
  filter in front of the store-side ring scan and an inline presence-bit test in
  `ReadU32`'s fast path, cutting the map's stage 13 cost by about 55% with identical
  binding; see "What the map costs, and the filter in front of it" there.

- `0013-settings-slot-in-section.patch` — `SettingsRegistry.Extend` only draws
  *after* a section's whole body, so a port option that belongs beside one of the
  runtime's own controls could only ever land in a block underneath the lot. This
  adds `SettingsRegistry.DrawSlot(slotId)` and one call to it in the display
  section, after the render scale, which is where the widescreen aspect goes.
  Register with `PatchSettings.RegisterSlot`, not `Register`. UI only — **no
  recompile**.

- `0014-gte-zbuffer.patch` — a depth buffer from the same recovered SZ3
  perspective correction already follows through memory. The GPU has none, so
  intersecting surfaces take turns in front of each other on the ordering table;
  both rasterizers now test the recovered view depth per pixel. Window depth is
  a fragment value, not clip-space Z, so OpenGL does not far-clip the already-
  projected triangle. A miss is painter's order, so the HUD is untouched. Off
  by default. See "Z-buffer" in `docs/RENDERING.md`. **No recompile** — the lookup is
  the one `0012` already does.

- `0015-zbuffer-occlusion-census.patch` — diagnostic behind `KF2_ZBUFFER_PROBE=2`.
  Every polygon's bbox, depth range, table position and flags for the window, the
  `DrawOTag` walk position (`GteDepth.OtEntry`, counted from the far end — so
  `Widescreen`'s replacement of `DrawOTag` has to publish it too), and a 32×16 map
  read back from the depth attachment. It reads the RT the depth batches went to,
  not the presented one (last frame's, under double buffering) and not the most
  recently drawn (a fill stamps `LastDrawFrame` too, so that can be a buffer just
  cleared). **No recompile.**

- `0016-zbuffer-clear-at-frame-head.patch` — `PresentDisplay` incremented `_frame`
  before its trailing `Flush`, so the depth clear (keyed on `LastDrawFrame !=
  _frame`) fired on the tail of the *outgoing* frame and was then skipped at the
  head of the next one, which inherited the last batch's depths. Nothing rescues
  it — `isbg=0` here, so no game-side fill reaches `FillRtFull`. Swapping the two
  statements took the depth-map readback from 67-of-91 empty to 11-of-11
  populated. Real and measured, but **it did not cure the sky showing through
  nearby walls** — a second cause remains. **No recompile.** See "The clear
  landed at the tail of the frame" in `docs/RENDERING.md`.

- `0018-imgui-fractional-framebuffer-scale.patch` — Silk's `ImGuiController`
  computes `io.DisplayFramebufferScale` by dividing two `int`s, so a compositor
  running a display at a *fractional* scale (KDE's 1.15) truncates to 1 and
  `RenderImDrawData` sizes its GL viewport and every scissor from the logical
  window instead of the framebuffer — the whole interface lands in the bottom-left
  corner, with dead margins top and right. Recomputed as a float between
  `Update()` and `Render()`, which is the only window where it is read: layout is
  already fixed and still logical, so **input is untouched**. An integer scale
  divides exactly, which is why a 1:1 monitor never shows it. Its sibling defect
  is ours and unfixed — `QueryDpiScale()` reads the *primary* monitor's content
  scale once at startup, and GLFW's Wayland path returns the integer `wl_output`
  scale, so a 1.15 monitor reports 2.0 and the chrome is oversized on both
  screens. **No recompile.** See "The interface only fits a monitor whose scale is
  a whole number" in `docs/RUNTIME.md`.

- `0017-mouse-capture-and-motion.patch` — `InputManager` owns the `IMouse` and is
  `internal`, so a port could not reach the cursor at all. Adds `MouseCaptured`
  (`CursorMode.Raw`, or `Disabled` where raw is unsupported — both make GLFW
  report an unbounded virtual position, which is what turns successive positions
  into motion), `TakeMouseMotion` and `IsMouseButtonDown`, forwarded from
  `HostWindow` beside the `IsKeyDown` that already plays that role for the
  keyboard, and gives the cursor back in `Shutdown`. Everything else about mouse
  look is `patches/Mouse.cs`. **No recompile.** See "Mouse look" in
  `docs/INPUT.md`.

- `0019-popups-cannot-leave-the-window.patch` — every popup is centred and pinned
  with `SetNextWindowPos`, which is the flag that suppresses ImGui's own clamp
  into the viewport, and its size (`Size * Theme.Scale`) is capped against
  nothing, with `NoResize`, `NoMove` and `NoScrollWithMouse` closing the ways
  back. The 780x500 settings popup therefore outgrows a 1280x720 window at a
  `Theme.Scale` of 1.44 and takes the UI-scale field — the one control that would
  undo it — off-screen with it, permanently, since the value is saved. Reachable
  from the slider alone (0.5-3), and reached at `UiScale` 1 on the monitor whose
  `DpiScale` misreads as 2.0. The size is now clamped to the viewport, so an
  oversized scale costs scrolling instead of the controls, and `Debug > Reset
  view` re-applies `FontGlobalScale` and `Theme` instead of leaving giant text
  behind small windows. **No recompile.** See "The scale can put the settings out
  of reach" in `docs/RUNTIME.md`; `patches/UiScale.cs` (`KF2_UISCALE`) is the
  port's own way back for a config already past that point.

- `0020-theme-apply-compounds-the-style.patch` — `Theme.Apply` ends in
  `ScaleAllSizes`, which multiplies *every* size field, but only resets some of
  them first, so each accent, background or scale change multiplies the rest
  again: measured, `WindowMinSize` 32 -> 44 -> 88 -> 528 -> 1056 over five calls.
  ImGui floors every non-child, non-`AlwaysAutoResize` window at `WindowMinSize`
  **after** applying a size constraint, so that overrides `0019`'s clamp and the
  popup grows off the bottom of the screen — a 1264x704 clamp measured coming out
  1264x1056 on a 1280x720 viewport. `Apply` now restores the style ImGui built
  before re-theming, which stays correct whatever upstream adds to
  `ScaleAllSizes`. Latent since long before `0019`; changing the *accent*
  compounds it too. **No recompile.** See "The scale can put the settings out of
  reach" in `docs/RUNTIME.md`.

- `0021-true-color-24bit-output.patch` — the console renders into 15-bit VRAM, so a
  smooth shaded fog gradient bands into 32 levels unless the ordered dither hides
  it with a crosshatch. Two things enforce the truncation: the `GlDisplayRt` colour
  attachment is `Rgb5A1`, and the fragment shader's `quant5` ends in
  `min(c8 >> 3, 31) / 31.0`. Under `GteDepth.TrueColor` the attachment becomes
  `Rgba8` and `quant5` keeps eight bits, so the gradient is smooth without the
  crosshatch. Textures stay 5-bit (they live in 15-bit VRAM), so only the shaded
  gradient gains precision; the writeback/present blits convert automatically.
  GL backend only — the software rasterizer is always 15-bit. Off by default (the
  authentic look). `patches/TrueColor.cs` (`KF2_TRUECOLOR`) is the switch and
  `patches/settings/ShadingPage.cs`'s combo is where it is chosen. **No recompile** —
  render-target format and shaders are runtime. See "True color" in
  `docs/RENDERING.md`.

- `0022-present-stale-wide-target.patch`, `0023-splash-margin-idle.patch`,
  `0024-margin-content-latch.patch` — the present gate. `PresentDisplay` picks a
  wide render target only when its margin columns have carried a world, so a scene
  that never draws out there (the MDEC boot splash) keeps its authored width
  instead of flapping between widths. Latching is per target, and it is cleared
  when an **executable** loads — from `patches/Widescreen.cs`, not from
  `Dispatcher.Load`, which fires for the `fdat` area modules too and put the black
  bars back for the length of every area transition. **No recompile.** See "The
  present gate" in `docs/WIDESCREEN.md`.

- `0025-frameclock-target-rate.patch` — `FrameClock.FrameMs` was a `const` 60 Hz,
  and it is the *host* rate: the emulated vblank grid (`LibEtc.VBlankMs`) and every
  game clock hanging off it are a different 60 that must not move, or the music
  speeds up. Now a settable `FrameClock.TargetFps` (0 = off), exposed as
  `Runtime.TargetFps` because `FrameClock` is `internal`. It still cannot be a
  frame pacer — it throttles per `VSync` *call* and a frame carries two — so the
  port hands it a permissive ceiling and keeps its own deadline at `DrawOTag`.
  **No recompile.** See "There were three fixed 60s" in `docs/RUNTIME.md`.

- `0026-str-pacing-without-a-latch.patch` — an STR movie is paced by the disc:
  sectors arrive at 150 a second, a frame is 9-14 of them, and the game's display
  loop blocks in `StGetNext` until one is complete, so the loop's own rate never
  enters into it. `StreamLoop` modelled that, but only once a latch tripped — two
  decoded frames sitting in the 32-slot ring at the same time — which for the third
  intro movie's 13-14-sector frames never happened, because the game drains a frame
  as soon as it is ready. Unthrottled, that movie played at the **render rate**
  instead: measured ~56 frames a second at `KF2_FPS=60` and ~93 at 144, against the
  10 the disc holds it to. Paced from the start of the stream now, since there is no
  free burst on hardware either. **No recompile.** See "The intro movie ran at the
  render rate" in `docs/RUNTIME.md`.

- `0027-commit-each-hook-independently.patch` — `HookManager.Commit`'s `foreach`
  installed each detour with no guard, so the first `new Hook` that threw abandoned
  every function after it in the dictionary. Silently: a patch that hooks from an
  `OverlayLoadedEvent` listener — which is all of them, since `SymbolRegistry` is
  only readable once the dispatcher's overlay tables are registered — has its
  exception swallowed by `Event.Dispatch` into one stderr line. Losing
  `FramePacing`'s frame boundary that way reads as *the whole game running fast*,
  because `Floor()` and `_tickThisFrame` both hang off it and `_tickThisFrame`
  fails open. Each function is committed on its own now and a failure names the
  method. **No recompile.** See "Everything hung off one hook" in
  `docs/PATCHES_AND_MODS.md`.

- `0028-hook-manager-commit-state.patch` — `AddPre`/`AddPost` only append a
  delegate and return `true`; the detour is created later in `Commit`, which since
  `0027` fails per function without throwing. A patch counting `Add*` returns
  therefore claimed what it had *queued* — `FramePacing` could print
  `boundary 3/3 DrawOTag + 3/3 VSync` with no boundary installed, latch itself
  done, and never retry, which is the uncapped-picture-and-8×-world failure.
  `IsRegistered` and `IsCommitted` let a caller read back what actually landed.
  **No recompile.** See "A registration is not a hook" in
  `docs/PATCHES_AND_MODS.md`.

- `0029-output-panel-image-rect.patch` — the game picture is not the window, and
  nothing outside `OutputPanel` knew where it was. It is an `Image` inside that
  panel, fitted to the panel's content region at the display's aspect and centred
  in it, so the menu bar, the dockspace border and any docked panel take their
  share off it and a 4:3 picture in a wider window has a bar either side. An
  overlay could therefore only anchor to the viewport, which is why the
  full-screen map covered the port's own chrome and lined up with neither. A
  public `OutputView` publishes the rectangle from the one place that computes it,
  once a frame, and is invalid when the panel drew no picture so a caller can fall
  back to the viewport. It also publishes **`GameW`/`GameH`**, the picture's size
  in the game's *own* pixels, off the `SetTexture` call that already receives
  both: `Min`/`Max` alone are a rectangle and not a scale, so nothing could turn
  a window pixel back into a game pixel — which is what the menu pointer needs to
  ask which item is under the cursor. UI only — **no recompile**. See "A dynamic
  map" in `docs/PATCHES_AND_MODS.md` and "The menu pointer" in `docs/INPUT.md`.
  Since amended: `OutputView.Hovered`, the picture's own `IsItemHovered`. The
  picture is an ImGui window, so `io.WantCaptureMouse` is true whenever the
  pointer is over it, and the remaster editor's click gate
  (`!WantCaptureMouse`) never opened: neither a pick nor a light placement on
  the picture did anything. The amendment is the second diff in the patch file.

- `0030-expose-host-pump.patch` — the shipped launcher has to build the game
  before there is a game to run, and that blocks for seconds; a window that stops
  pumping for seconds is one the desktop offers to force-quit. `HostWindow.Pump`
  already does exactly the right thing and is `internal`, and `WaitForValidDisc`
  already runs that loop but only ever for its own condition. Exposed as
  `Runtime.Pump`. Everything else the progress UI needs was public already —
  `Popup` is abstract-public and `PopupManager.Register` takes any implementation
  — so `Verdite2.Launcher/BuildProgressPopup.cs` is not a patch. UI only, **no
  recompile**. See "The one patch this needed" in `docs/PACKAGING.md`.

- `0032-expose-pad-queries.patch` — `InputManager` is `internal`, so a port
  drawing its **own** binding table could not ask whether a pad is connected or
  what is held down on it. Both methods were already `public` on that class, so
  unlike `0017` nothing had to be added there — only the two forwards from
  `HostWindow`, beside `IsKeyDown` and the mouse block. Two lines against `0017`'s
  sixty. UI only — **no recompile**. See "The Input pane is the port's" in
  `docs/INPUT.md`.

- `0031-output-panel-fills-its-dock-node.patch` — the picture is the point of the
  Output panel, so it gets none of the chrome every other panel wants. The themed
  `WindowPadding` (12,10) and the 1px `WindowBorderSize` are read by `Begin` when
  it computes the inner rect, so a docked, tab-bar-less Output panel filling the
  dockspace still letterboxed the game behind a band of window background on all
  four sides — scaled by `Theme.Scale`, so widest exactly where the DPI is misread
  highest. Both are pushed around `Begin` only and popped straight after it, so
  the toasts drawn below still lay themselves out on the real style and no other
  panel is affected. UI only — **no recompile**. See "The picture is inset
  inside its own panel" in `docs/RUNTIME.md`.

- `0033-sans-serif-interface-font.patch` — ImGui's built-in face is ProggyClean,
  a 13 px bitmap: it is pixel art, it does not scale (every other size is a
  stretched bitmap), and it makes the port's own settings window read as a debug
  overlay laid over the game. This is upstream's own fix back-ported —
  RecompOne `aaf7be0`, which our pin `870c5ba` predates — so `Icons` becomes
  `FontSet`, Noto Sans is embedded and merged with the Font Awesome range, and
  the size goes 13 → 16 px. **Upstream's CJK face is deliberately not carried**:
  it is a second 16.5 MB resource, and every string in the runtime's three
  languages (en, pt-BR, es-419) is Latin, so it would cost 16 MB in every release
  artifact to render nothing anyone can select. Cyrillic, Greek and Vietnamese are
  kept, being Noto's own coverage and only atlas space — they are what a path or a
  mod name falls back to instead of boxes. A missing resource falls back to the
  bitmap font rather than to no text. The font is OFL 1.1
  (`patches/recompone/assets/NotoSans-OFL.txt`, which the packaging must ship).
  UI only — **no recompile**. This is the one patch that *wants* to stop applying:
  when the pin moves past `aaf7be0` it is upstream's, and the right response to
  `FAILED TO APPLY` here is to delete it. See "The interface's font" in
  `docs/RUNTIME.md`.

- `0034-pgxp-value-tracking.patch` — **upstream's PGXP, backported.** RecompOne
  grew a real PGXP after our pin (`39fb337a`, `91c20fcf`, `95f0585b`, `6aae910a`,
  2026-08-31 to 09-07): the GTE's own divide publishes a float screen position and
  view depth, and those follow the value through the CPU's registers and a
  `PgxpValue`-per-word RAM shadow to the GP0 packet. `RecompOne.Runtime/Pgxp/` is
  verbatim from `6aae910a` apart from living one directory up, beside `GteDepth`
  rather than under it; the edits are `Gte.Rtp` publishing the precise vertex,
  `Nclip` doing backface culling on precise positions, the transform-serial ring,
  `PSMemory`'s ctor sizing the shadow and `Runtime.Run` initialising the vertex
  cache. **Upstream's frame interpolation is deliberately not carried** — it is a
  separate experimental feature that arrived in the same commit. Inert until
  something turns it on. **No recompile.**

- `0035-pgxp-cpu-hooks.patch` — the recompiler half, and one of three patches that
  **force a recompile**, with `0004` and `0037`. `InstructionEmitter` emits
  `if (Pgxp.CpuTracking) PgxpCpu.X(...)` beside every load, store, move, shift,
  add, multiply and divide, which is what makes PGXP's coverage a fact rather
  than a rate. The gate is emitted rather than taken inside the hook, so with PGXP
  off the cost is a predictable branch. Loads and stores hold the address in a
  local rather than emitting the expression twice — upstream evaluates it again
  after the access, which hands the hook the wrong address for `lw $t0, 0($t0)`.
  Measured: 68,188 hook sites in `game.cs`, no change in generated line count
  (the hooks append to existing lines), build 15 s → 37 s.
  Since amended: the branch was not free (area 1 frame work 1.05 ms against 0.91
  without it), so the emitted test is `PgxpGate.Cpu && Pgxp.CpuTracking`, with
  `PgxpGate.Cpu` a `static readonly` the JIT folds to `false` unless `KF2_PGXP=1`
  armed it at boot. See "The PGXP gates" in `docs/DEVELOPMENT.md`.

- `0036-pgxp-vertex-and-depth-source.patch` — where the two mechanisms meet.
  `DrawPolygon` asks PGXP when it is on and `GteVertexMap` when it is not, filling
  the same `Vert` fields either way, so `HleVertex`, both rasterizers and the
  shaders are untouched by the choice. Three things are ours rather than
  upstream's: the **tolerance is actually spent** (upstream defines
  `pgxp.tolerance` and never reads it — here a recovered position more than that
  many pixels from the packet's is refused, because it is a different vertex
  rather than a better version of this one), **`PgxpStats`** counts where each
  answer came from, and **a depth-tested triangle is given a real clip W whether
  or not its texture is being corrected** — `vDepth` is an ordinary varying, so
  with `w = 1` an untextured wall's interior depths came out linear in screen
  space when it is `1/z` that is affine there. The software rasterizer had always
  interpolated the reciprocals; this makes the GL path agree. Also the
  **depth-clear threshold** (`GteDepth.DepthClearThreshold`, DuckStation's 300),
  which bumps the existing `Generation` rather than adding a clear path. **No
  recompile.**

- `0037-chd-disc-images.patch` — **upstream's CHD support, backported** (`137a793`,
  six commits past our pin). `CueFs` becomes `DiscFs` over a new `IDiscImage`, with
  `CueBinImage` and a from-scratch libchdr port (`Cdrom/Chd/`: header, hunk map,
  Huffman, LZMA, FLAC, CD-sector ECC) behind it; `DiscImage.Open` picks by
  extension and falls back to the CHD magic, so the recompiler, the runtime and the
  launcher only changed a type name. The commit's unrelated **RAM-size** change
  comes with it — `PSMemory(uint ramSize)` and `Runtime.RamWordMask` replacing the
  literal `0x1FFFFCu` — and nothing here passes a size, so the RAM is the same 2 MB
  and the mask the same value. Codecs: cdzl, cdlz, cdfl, zlib, lzma; **not zstd**,
  and a `cdzs` image is refused at the picker rather than crashing. **Forces a
  recompile** — `EntryWriter` emits `DiscFs.Open`. Measured: `generated/` from the
  CHD is byte-identical to `generated/` from the cue; the recompile costs
  1.35-1.39 s against 0.86-0.89 s; the autostart area load is 305.1 ms against
  305.8 ms, the same 84 steps over the same 105 blocking VSyncs; a CHD run walks
  `open` → `game` → `fdat02` → `fdat05` at 144.0 fps / 20.0 ticks/s, and the intro
  STR decodes. See "CHD disc images" in `docs/RUNTIME.md`.

- `0038-expose-the-mouse-wheel.patch` — `InputManager` owns the `IMouse` and is
  `internal`, so a port could not read the wheel at all; nothing in the runtime
  listened for the `MouseEvent` that carried it either. Adds `TakeMouseWheel` in
  the drained shape `0017`'s `TakeMouseMotion` already has — scroll arrives as
  discrete events, so a drained accumulator cannot miss a notch or spend one
  twice, where ImGui's per-frame `io.MouseWheel` is a level and a menu loop
  iterating at 30 a second against a 144 fps window would do both. The value is a
  **float** throughout: `OnScroll` had `Wheel = (int)wheel.Y`, exact for a
  discrete wheel and a total loss for a trackpad's two-finger scroll, which
  arrives in fractions of a notch. `patches/MenuMouse.cs` is the only caller.
  Input only — **no recompile**. See "The wheel owns the page, not the cursor" in
  `docs/INPUT.md`.

- `0039-render-scale-survives-a-menu.patch` — a modal sub-loop keeps the world
  behind it by reading the finished frame out of VRAM once (`StoreImage`) and
  blitting it back every iteration (`LoadImage`), and that roundtrip is 1x by
  construction: VRAM is the console's own resolution, so at any render scale the
  restore stamped a 1x picture over the display area on every frame of the menu,
  a shop or an NPC's message box. Only the *middle* of the picture, because
  `Writeback` copies a target's middle `W` columns and the widescreen margin
  lives nowhere but in the render target — which is the seam the report named.
  `ReadVram` now also takes a scaled copy on the GPU and `WriteVram` serves an
  upload of byte-identical pixels from it. Keyed on content and size rather than
  address, since the frame may be restored into either display buffer; anything
  the game actually built in RAM fails the compare and uploads as before.
  Measured in the menu at 144 fps, 16:9, scale 4: 120 restores per two seconds
  and **0** misses, present still `wide`, world still 20.0 ticks/s; `0, 0` in an
  area, so it costs nothing when nothing reads the frame back. `KF2_VRAMSNAP=0`
  is the comparison; no control in the window, a render scale surviving a menu
  not being a choice. GL backend only. **No recompile.** See "The render scale
  did not survive a menu" in `docs/RENDERING.md`.

- `0040-ambient-occlusion.patch` — contact shading in the corners, from the same
  recovered SZ. **The interesting part is not the SSAO, it is where the G-buffer
  comes from.** A screen-space pass needs the nearest visible surface at every
  pixel before any shading happens, and this port cannot build one the usual way:
  the geometry arrives incrementally through GP0, nothing holds it, and nothing
  knows the frame is finished until it is, so there is no moment at which a depth
  prepass could run. Painter's order supplies it instead — `DrawOTag` walks the
  ordering table back to front, so give every 3D triangle a depth *write* with the
  test left at `GL_ALWAYS` and the attachment ends the frame holding exactly the
  visible-surface depth, with nothing rejected and the ordering table still in
  sole charge of what is visible. That one `DepthFunc` is the whole difference
  from the Z-buffer, which is why `GteDepth.DepthWanted` replaced
  `GteDepth.ZBuffer` at every site that decides whether a depth is recovered —
  including `GpuRaster`'s `wantZ` and the `tex || Subpixel || ZBuffer` gate above
  it, without which the buffer holds only the *textured* geometry and most of this
  game's architecture is flat-shaded. **The far plane is the HUD mask and it is
  free**: everything with no recovered depth writes `1.0` instead of the clip Z it
  used to, so the HUD, the menus and any triangle the vertex map missed are
  neither shaded nor allowed to occlude, and semi-transparent primitives write
  nothing at all, so a death fade or a damage flash does not switch the shading
  off for the frames it covers. Two full-screen draws at present (spiral SSAO with
  a 4x4 interleaved rotation, then the 4x4 depth-aware box that cancels it
  exactly) into the pass's own RG8 texture, which the present shader multiplies —
  so nothing the game can read back carries the shading, not VRAM, not either
  display buffer, and not the frame a modal loop restores. The pass undoes the
  game's own projection to get a view position out of a depth texel, with `H` and
  the `OFX`/`OFY` centre read off `Gte.Rtp` — measured `H 200`, not the 320 a guess
  would have used. **The one error worth recording was silent and intermittent**:
  the first version added the drawing offset of the last depth-writing triangle,
  which belongs to the buffer being *drawn* while the pass runs against the buffer
  being *presented*, so with two display buffers half the frames put the
  projection centre a whole screen out; a display target's offset already **is**
  its own origin, or `uPosBias` would be misplacing every polygon. The probe
  prints the reconstructed centre for that reason and it should read `0.500,0.500`.
  `KF2_AO_PROBE=2` is the counter that matters — every other number stays
  identical if the shader returns white on every pixel — and it reads the
  occlusion back as a darkest value, a mean, a shaded share, a surface share and a
  32x16 map. Off by default, for the sub-pixel reason and because it is
  deliberately not authentic; one checkbox under Video ▸ Enhancements and the
  tuning on the console. GL backend only. **No recompile.** See "Ambient
  occlusion" in `docs/RENDERING.md`. Since amended: `GteDepth.AoResolution` caps
  the pass, the blur and the normal buffer at a multiple of the game's pixels, and
  the two occlusion textures are R8 unless the probe is on; see "What the pass
  costs" there.

- `0041-anisotropic-filtering.patch` — a screen pixel covers an *area* of the
  texture, and the shape of it is the parallelogram spanned by the two screen
  derivatives of the texture coordinate: about a square square-on to a wall, and a
  long thin sliver on a floor running away to the horizon. The console read one
  texel out of that sliver, and which texel changes completely for a sub-pixel
  movement of the camera — the crawling, sparkling floor. **The interesting part is
  where the filter had to go.** `GL_TEXTURE_MAX_ANISOTROPY` on the VRAM sampler does
  nothing at all: the game's textures are never sampled by a GL sampler, the shader
  `texelFetch`es a sheet holding every page and every CLUT at once (so no filter may
  run across it), and in the 4- and 8-bit modes the value read is a CLUT *index*
  whose average with its neighbour is an unrelated colour. So `decode(raw)` holds
  the whole per-texel job — texture window, page wrap, nibble extract, CLUT lookup —
  and the kernel calls it once per texel along the long axis, up to `uAniso`, and
  averages. The single-sample path calls the same function, so off is bit-identical
  to before. There is **no mip chain and there cannot be one** for that same first
  reason, so this is supersampling rather than mipmapped anisotropy, and a footprint
  large on both axes is still averaged along one only. Two things the encoding
  forces: a transparent texel is stored as black, so taps are weighed by solidity
  and renormalised, discarding below half coverage; and the semi-transparency bit
  picks a blend equation rather than being a colour, so it comes whole from the
  centre tap. **No "is this 3D" test is needed** — a 2D primitive is axis-aligned
  and unminified, so the tap count is 1 and it takes the unfiltered path by
  construction, which is why this needs no varying and does not care whether
  perspective correction is on. Both prim shaders, core profile and GLSL 120 (whose
  loop is a constant bound with a `break`, 1.20 not promising dynamic bounds); GL
  backend only, native VRAM paths only. `GteDepth.AnisotropyLive` is read back from
  the one place that uploads the uniform. Off by default. **No recompile** — a plain
  uniform the next batch reads. See "Anisotropic filtering" in `docs/RENDERING.md`.

- `0042-present-counters.patch` — `FramePacing` paces from hooks on `VSync` and
  `DrawOTag`, so it cannot use those hooks to notice that they have stopped
  running. Some boots present at exactly twice the asked-for rate for the whole
  session, which is the host ceiling `FramePacing` hands `FrameClock` with nothing
  of the port holding the picture. `LibEtc.VSyncCalls` and `LibGpu.AutoPresents`
  count presents in the bodies themselves, and `LibEtc.CaptureNextStack` returns
  the managed stack of one call, which shows whether it still came through
  `HookManager.Invoke`. Read by `FramePacing`'s sentinel and its probe line.
  **No recompile.** See "The smoothing is sometimes dead for a whole session" in
  `docs/TODO.md`.

- `0043-spu-audio-quality.patch` — the port's first change to sound. The reverb
  network ran on every other raw sample and held its output, where the hardware
  decimates and interpolates through a 39-tap half-band FIR; `Spu.ReverbMode`
  `Hardware` adds both (measured: wet level unchanged, 24 dB less energy above
  11 kHz, 37 dB less above 16 kHz). `Enhanced` swaps the network for an 8-line FDN
  (`Hardware/SpuReverb.cs`) sized from the game's own reverb registers, keeping
  every send and return level the game's. `Spu.Interpolation` adds Catmull-Rom and
  a pitch-band-limited 8-tap sinc (`Hardware/SincKernel.cs`) beside the Gaussian,
  which needed `Voice.Buf` widened to seven samples of history; `XaAudio` follows
  the same setting. `Host/Audio.cs` asks OpenAL Soft for its best resampler and
  counts underruns and stalls in `AudioStats`; `Spu.Stats` counts clamps, voices
  and mixer time, and `Spu.Mixed` hands the mix and the wet return to a listener.
  Defaults are the old behaviour: Gaussian, `Legacy`. **No recompile.** See
  `docs/AUDIO.md`.

- `0044-spu-positional-voices.patch` — the SPU renders a voice from a position
  when the port asks. The game pans a 3D sound once at key-on, folded front to
  back, and a hook cannot change that after the fact without owning the voice:
  `Voice` gains a key-on count (`KonSerial`, bumped per bit in `KeyOn`) and a
  `SpuSpatialVoice`, and `Tick` renders a voice through it only while its tag
  names the current key-on — so a voice reused by the music is its registers'
  again with nothing to clear. `SpuSpatialVoice` is mono in, rear low-pass, a
  fractional delay and a Brown-Duda head-shadow shelf per ear, a level per ear,
  all smoothed per sample; levels are in the game's 0..127 units and scaled by
  the registers' magnitude, so the VAB volumes carry. `CopyKeyOnSerials` and
  `SetSpatial` are the whole interface, and `Stats.SpatialVoicesPeak` counts it.
  `patches/PositionalAudio.cs` is the only caller. **No recompile.** See
  "Positional audio" in `docs/AUDIO.md`.

- `0047-gte-fast-path.patch` — the GTE ops this game calls in its polygon
  assemblers (`NcdsOp`, `NcdtOp`, `NccsOp` at `sf=12 lm=1`; `Dpcs`, `MvmvaOp`'s
  RotTrans form and `Rtps` at `sf=12 lm=0`) take a path with the shift, the
  saturation floor and the flag bits constant and the flags gathered in a local,
  3-4x faster an op and bit-identical over 3.6M random-state ops; `Rtp`'s bookkeeping
  after the divide is one method both paths call. A lighting op's two matrix
  products are remembered per normal until a write to control registers 8-20.
  `Gte.State`, `Save`, `Load` and `Diff` let `KF2_POLYASM=verify` restore and compare
  the GTE. Three small public reads for the port: `PSMemory.DirectRam` (a narrow
  store would do nothing but the store), `Dispatcher.HasPending` and
  `Interrupts.SlowPolls`. `KF2_GTE_FAST=0` and `KF2_GTE_LIGHTCACHE=0` are the
  comparisons. **No recompile.** See "The GTE fast path" in
  `docs/PATCHES_AND_MODS.md`.

- `0048-per-pixel-lighting.patch` — `Gpu/GteLightMap.cs`, a side table keyed by
  packet address that the port fills with what each packet's colours were made from:
  per corner a lit colour (or three light dots) and the raw depth cue, per packet the
  curve, the light colour and the two words it is checked by. `DrawPolygon` looks it
  up by the command word's source address and hands the values through `HleVertex`;
  `GlCore` uploads them in a second vertex buffer, only for a batch that has them,
  with `BK`/`LCM` as uniforms by generation (`FlushReason.StateLight`); `PrimFs`'s
  `shade8()` evaluates the depth-cue curve and the diffuse light per pixel, and
  returns the vertex colour unchanged with no record. `Gte.LightProducts` and
  `Gte.LightDots` give the port a normal's lighting without touching a register.
  GL core backend only. **No recompile.** See "Per-pixel lighting" in
  `docs/RENDERING.md`.

- `0049-gte-depth-quotient.patch` — `Gte.DepthQuotient(sz3)`, the `H/SZ3` divide
  `Rtp` feeds its depth cue, split out of `Divide` with no flag raised and no
  register touched. `Divide` now calls the same `Quotient`, so the op is unchanged.
  `EvenFog` needs it to evaluate a neighbouring tile's DQA at a vertex without
  running a GTE op. Checked: the port's IR0 from it matched the GTE's on about 1.2M
  vertices. **No recompile.** See "Fog changes at a tile edge" in
  `docs/RENDERING.md`.

- `0050-packet-depth.patch` — `Gpu/GtePacketDepth.cs`, a side table keyed by packet
  address that the port fills with each packet's four corner depths, checked by the
  command word and the first and last vertex words. While the port turns it on and a
  depth consumer is on, `DrawPolygon` gives a polygon a depth from its record or
  none, so the depth buffer holds only what the C# assemblers recorded (map tiles,
  clipped fans, models) and everything else keeps painter's order. W and the
  sub-pixel fraction still come from the address map. A record also carries
  `Solid`, which the port sets on a blended object-table model (the secret door):
  `HleVertex.Solid` carries it to `GlCore`, which draws it as zMode 4, depth-only
  with every texel for the occlusion pass (`uOpaqueDepth` 2). **No recompile.** See
  "The assemblers write the depth" and "A secret door is solid all the way through"
  in `docs/RENDERING.md`.

- `0051-coplanar-depth-tolerance.patch` — a tested fragment compares a depth pulled
  towards the camera by `GteDepth.DepthBias` SZ units plus `DepthSlope` times its
  per-pixel slope (`uDepthBias`, `uDepthSlope` in both prim shaders), so two
  coplanar surfaces go to the later table entry instead of fighting. The bias is
  never written: `GlCore.Flush` draws an opaque tested batch's true depth first with
  colour masked, then its colour with the bias and no depth write
  (`GteDepth.ZPrepasses`). The software rasterizer tests with the constant and keeps
  the nearer depth. At zero bias and slope the shader output is bit-identical to
  before. **No recompile.** See "Coplanar panels fought at the seam" in
  `docs/RENDERING.md`.

- `0052-vertex-map-peek.patch` — `GteVertexMap.Peek`, `TryGet` without the hit and
  miss counters, so `PolyAssembler`'s backface cull can read a cached vertex's
  fraction without moving the perspective probe's hit rate. The census behind
  `KF2_SUBPIXEL_PROBE` (`GpuRaster.SubCensus`, `GteDepth.Census*`) arrived with it.
  **No recompile.** See "A thin face was culled on whole pixels" in
  `docs/RENDERING.md`.

- `0053-fluid-scroll.patch` — both prim shaders gain `decodeFluid()`: matching a
  fragment's VRAM coordinate against up to eight dest RECTs, shifting V by a
  leftover phase and blending the two wrap-rows, so a scrolling texture (water,
  slime skins) moves between the integer uploads `func_8002DC78` left in VRAM.
  `uFluidN` of 0 is the centre sample unchanged. `GteDepth.Fluid*` holds the rects
  and offsets; `GlCore` uploads them per batch. GL only. **No recompile.** See
  "The water still steps at the tick" in `docs/PATCHES_AND_MODS.md`.

- `0054-vram-sample-1x.patch` — the prim shader samples a 1× VRAM texture, and
  `WriteRect` (`LoadImage`) stops blitting each upload up to the scaled atlas.
  That blit wrote the atlas as an FBO colour attachment; the next `GlCore.Flush`
  sampled it and the driver waited — 0.76 ms on the first polygon after
  `func_8002DC78`'s ten uploads, against 0.2–5 µs for every other send. A draw
  with no display target used to land in the atlas and then `Publish` its AABB
  onto 1×, which erased the uploads (the atlas no longer holds them); those
  draws now land in 1× and `Promote` up. Writeback of a display target still
  `Publish`es. Dest copies / `TextureBarrier` run only when the batch actually
  samples dest (the mask bit, the 2.1 blend path, or blend mode 2). The 1×
  texture is also never a draw attachment: GPU writes go to a second 1×
  framebuffer and `CommitDraw` copies the AABB back, because leaving sample
  VRAM on `SampleFbo` made the `TexSubImage2D`s render-target writes and left
  `Flush` at 0.6 ms after the atlas blit was gone. GL only. **No recompile.**
  See "Watching a frame being built" in `docs/DEVELOPMENT.md`. Since amended:
  an upload was promoted from 1× to the scaled framebuffer only when **no display
  target existed at all**, but that framebuffer is what the present reads
  whenever no target *serves* the display — none covers it, or the margin latch
  refuses the one that does. Boot's first `isbg` clear leaves a 640x240 target at
  `(0,240)` that nothing draws into again, so for the ~300 presents it lives every
  MDEC frame of `OP0.S` (the ASCII Entertainment logo) missed the screen: the
  jingle played over black, at 16:9 and at 4:3. `WriteVram` now promotes any
  upload no serving target contains (`ServedByTarget`), and the present's latch
  test is the same `MarginRefused`. The amendment is the second diff in the
  patch file. See "The first intro movie never reached the screen" in
  `docs/RENDERING.md`.

- `0055-append-batch-vertices.patch` — `GlCore.FlushCore` uploaded every batch's
  vertices to offset 0 of `_vbo` (and `_vboLight`), the range the previous batch's
  draw was still queued against, so the driver waited on each upload: 2.55 ms a
  frame over the 687 batches of a view of `fdat02`'s water. Batches now append at
  `_vboCursor` and draw from there, and both buffers are orphaned when it wraps.
  `uScale`'s location is cached rather than looked up by name per batch, and the
  fluid uniforms (`0053`) are sent only when they change. The picture is the same
  by the depth map and the occlusion readback. **No recompile.** See "Water on
  screen cost 5 ms a frame" in `docs/DEVELOPMENT.md`. Since amended: the cached
  upload was `Uniform1(loc, _legacy ? (float)s : s)`, and C# gives that conditional
  the type `float` whichever branch is taken, so the core shader's `int uScale`
  refused every update with `GL_INVALID_OPERATION` and kept the value set at init.
  Its only reader on the core path is the dither grid, so a render-scale change or a
  1x draw put the *Dither* shading's pattern at the wrong size; *Smooth*, the
  default, skips it. Found by `0065` on its first run. The amendment is the second
  diff in the patch file.

- `0056-ram-size-above-2mb.patch` — the port gives the guest 4 MB so the game's
  primitive buffers can move above 2 MB. `GteVertexMap` sized its tables from the
  run mode (2 MB retail), so an address above that would alias onto the low 2 MB;
  it sizes from `Runtime.RamSize` now, and `PSMemory`'s constructor reallocates
  them when a patch switched the map on before the RAM was made. `RamProbe`
  counts accesses above 2 MB per 64 KiB page in the seven RAM fast paths, behind a
  `static readonly` the JIT folds away unless `KF2_RAM_PROBE=1`. **No recompile.**
  See "The primitive buffer ran out" in `docs/WIDESCREEN.md`.

- `0057-snapshot-from-display-target.patch` — `0039` took a readback's scaled
  copy from the atlas, which `0054` stopped keeping current, so a restore of a
  texture-space readback wrote old texels over the textures (a shop). The copy is
  now taken only from a display target that covers the rectangle; any other
  readback uploads at 1x. Also `VramCheck` (`KF2_VRAMCHECK=1`), a CPU mirror of
  VRAM checked after every VRAM operation. **No recompile.** See "A shop
  overwrote the textures with the atlas's old texels" in `docs/RENDERING.md`.

- `0058-ao-geometry-normals.patch` — the occlusion pass's normals come from the
  frame's own geometry instead of from four depth texels. `Gpu/AoGeometry.cs` keeps
  each depth-carrying triangle as `GlCore.DrawTri` submits it, per display target
  (the presented target was drawn a frame ago, which is why the depth attachment
  lives there too); `GlCore.RenderNormals` (`RenderSurfaces` since `0067`) draws the list again after the frame,
  with no depth test and no depth write, so **order** is what makes it agree with
  the depth buffer rather than a test that could disagree. `NormalVs`/`NormalFs` are
  `PrimVs`'s position arithmetic to the letter with the view depth as W, and the
  plane's normal is the cross product of the reconstructed view position's two
  screen derivatives — taken *inside* one primitive, so it can never straddle a
  silhouette. It cannot be an MRT off the colour pass: `PrimFs` has a dual-source
  output for the console's blend modes and such a program may not render to more
  than one draw buffer. A pixel the buffer did not reach keeps the old
  reconstruction, by alpha, so it is additive; the AO texture gains a blue channel
  saying which of the two answered, because every other number reads the same with
  an empty buffer. `KF2_AO_NORMALS=0` is the comparison. Measured in area 1 at 144
  fps: 18,309 tris/s kept, 142.4 normal passes/s, 100.0% of the covered picture lit
  from a geometry normal, 144.0 fps drawn at 20.0 ticks/s either way. **No
  recompile.** See "The normal was the guess" in `docs/RENDERING.md`.

- `0059-world-space-occlusion.patch` — the occlusion pass also marches the area's own
  80x80 tile grid, so a wall behind the camera occludes as one in front of it does,
  which is the thing a screen-space pass structurally cannot do. `GteDepth` carries
  the camera's rotation and world position and the grid as a texture; `GlCore`
  uploads it when the port's generation moves and hands the pass the matrix
  **untransposed**, because GLSL reads a `mat3` column-major and that is the inverse
  the pass wants. `AoFs.worldOcclusion` takes eight directions by three steps,
  weighted by how much of the surface faces the horizon it found and averaged over
  every direction, so floors darken near walls rather than not at all. The port half
  is `patches/AoWorld.cs`. Off by default. Measured in area 1 at 144 fps: the shaded
  share of one view 17.8% -> 34.4%, darkest 0.69 -> 0.64, 144.0 fps drawn at 20.0
  ticks/s. **No recompile.** See "Occluders the camera cannot see" in
  `docs/RENDERING.md`.

- `0060-texture-rect-and-mip-atlas.patch` — every texture filter tap is held inside
  the polygon's texture rectangle, and mipmaps are built where a texture is
  decoded. `HleVertex.TexRect`: the bounding box of the polygon's UVs, or for a
  clipped fan the face's, from `Gpu/GteTexRect.cs` — a side table by packet
  address the port fills after `func_800302E8`. `GlCore` uploads it with an atlas
  entry in a third vertex buffer (location 10, only for a batch that has them);
  `Backends/Common/GlTexCache.cs` is a 2048x2048 RGBA8 atlas with levels 0-8, a
  buddy allocator of power-of-two blocks, a decode pass through the CLUT and a 2x2
  box per level, run at the start of the batch that asked and invalidated by
  `VramTracker` (which gains `Clock`). `PrimFs` clamps the plain kernel's taps to
  the rectangle and adds `mipFootprint`: `n` taps over the whole long axis, each
  trilinear, with level 0 the exact texel. `GteDepth.Mipmaps`, `MipmapsLive`, the
  `Mip*` counters. The committed kernel leaked up to 99/255 of a red border into
  pixels inside the rectangle; this leaks 0. GL core only. **No recompile.** See
  "The taps still left the texture at its edge" and "Mipmaps where the texture is
  decoded" in `docs/RENDERING.md`.

- `0061-window-icon-sizes-and-app-id.patch` — two things one icon needs.
  `SetWindowIcon` was handed exactly one image, so a desktop asking for 32 or 48
  pixels got a window manager's resampling of whatever size it was given;
  `SetIcons` takes several and `_pendingIcon` becomes `_pendingIcons`, applied at
  `OnLoad` as before, with the single-image `SetIcon` now one call into it. And
  **GLFW was telling the compositor nothing about what this window is**: measured
  with `WAYLAND_DEBUG=1`, the toplevel sent `set_title` and **no `set_app_id` at
  all**, so on Wayland — where `glfwSetWindowIcon` is a documented no-op, the
  string `Wayland: The platform does not support setting the window icon` being in
  the binary — KWin had nothing to match a desktop entry against and could not have
  shown an icon whatever the port did, the shipped AppImage's own included.
  `HostWindow.AppId` (`Runtime.AppId`, set before `Initialize`) is hinted at window
  creation as `GLFW_WAYLAND_APP_ID` — the raw `0x00026001`, because Silk 2.22 has
  no name for a GLFW 3.4 hint — and as the X11 class and instance name beside it.
  Measured after: `xdg_toplevel#45.set_app_id("verdite2")` on the wire. What wants
  both is `patches/CardIcon.cs` and `patches/DesktopEntry.cs`. UI only — **no
  recompile**. See "The icon comes off the disc" in `docs/PACKAGING.md`.

- `0062-one-named-pad-button.patch` — `GetFirstPressedPadButton` sweeps the enum
  from zero and returns the lowest index held, which is what a binding table's
  "press a button" prompt wants and useless to anything asking about one
  particular button: SDL's `Misc1` (15) — the DualSense's mute key, an Xbox
  Series pad's share button, the one button a modern pad has that no PlayStation
  layout claims — is masked by anything else down. `InputManager.IsPadButtonDown`
  asks about one binding in the same encoding, so the triggers (100/101) and the
  stick directions (102-109) answer through it too, and `HostWindow` forwards it.
  `mods/kf2debug` is the caller: the mute key toggles noclip. Input only — **no
  recompile**. See "What the runtime had to grow" in `docs/INPUT.md`.

- `0064-swap-interval-and-wayland-vsync.patch` — the VSync setting asked GLFW for
  interval -1 (adaptive), but Silk's own `WindowOptions.VSync` / `IWindow.VSync`
  put interval 1 back over it, so 1 is what ran. On Wayland a swap on a hidden
  surface waits in `eglSwapBuffers` until the window is shown again, and the game
  presents from inside its own `VSync`, so **minimising the window stopped the
  whole game**. Silk's VSync is now always false and `ApplySwapInterval` owns the
  interval: 1 with VSync on, except on Wayland (`glfwGetPlatform`, which Silk 2.22
  does not bind), where the swap stays at 0 — the compositor never tears — and
  `FrameClock.WaitRefresh` holds one present per monitor refresh on the CPU
  (`Profiler.VSyncWait`). `FrameClock.VSync` now means "the swap blocks". **No
  recompile.** See "Minimising froze the game on Wayland" in `docs/RUNTIME.md`.
  Since amended: Silk applies its own `VSync` lazily, inside the first `DoRender`
  after it is set, so holding it false wrote interval 0 over `OnLoad`'s 1 before
  the first frame. **VSync on was interval 0 from boot off Wayland**
  (`wglGetSwapIntervalEXT` read 0 every frame on Windows, 112 fps drawn on a 60 Hz
  monitor, and tearing) until the setting was toggled in play. Silk's `VSync` is now
  set to agree with the interval `ApplySwapInterval` chose. The amendment's hunk is
  in `0066`'s file, where it shares a hunk with the deferred swap. See "VSync on Windows" in `docs/RUNTIME.md`.

- `0065-gl-debug-output.patch` — nothing in the GL backend read an error back, so a
  driver that refused a framebuffer or a call left that pass empty with no line
  anywhere. `Host/Window/GlDebug.cs`: `KF2_GLDEBUG=1` adds `ContextFlags.Debug` to
  every context asked for and installs a synchronous `KHR_debug` callback (GL 4.3
  or the extension); a context without one polls `glGetError` once a present
  instead. A message repeated every frame prints three times and then at each
  power of ten, with its count; `=2` adds notifications and the managed stack of
  each first report. Off, nothing is installed. The `Present` hunk carrying
  `GlDebug.Poll` is in `0064`'s file, where it shares a hunk with the refresh wait.
  **No recompile.** See "The GL backend reported nothing" in `docs/DEVELOPMENT.md`.

- `0066-vsync-without-the-driver.patch` — how VSync is kept off Wayland, and the
  port takes the swap from Silk to do it (`ShouldSwapAutomatically` off; every
  caller of `DoRender` goes through `HostWindow.RenderFrame`). **On Windows, in a
  window the compositor presents, the driver's interval is not used**: at interval
  1 an integrated Radeon fell into stretches of 30-55 fps (its swap blocks until
  the flip it queues), where VSync off in the same minute held 60.0. The interval
  stays 0, the frame is composed and flushed, and the swap waits on the kernel's
  vblank event for the window's monitor (`Host/Window/VBlankWait.cs`,
  `D3DKMTWaitForVerticalBlankEvent`, reopened on a window move; a failed wait falls
  back to the interval): 99-100% of frame intervals within a millisecond of 16.7 ms
  in four runs alternated with the interval. GLFW's fullscreen bypasses the
  compositor and tore that way, so the patch adds a **Borderless** display mode
  (Video ▸ Display mode, `ViewConfig.Borderless`, three new localisation keys and a
  hint), takes the wait there and windowed, and leaves Fullscreen on the interval.
  **Elsewhere the interval's swap is deferred** to the start of the next
  present, so the frame's GPU work — the occlusion pass and the composite, issued
  at present — overlaps the next frame's game code as it does with VSync off: 60.0
  fps deferred against 56.0 immediate on Windows with SSAO on High; the pad poll
  puts a waiting frame on the screen once the game has not presented for 34 ms.
  `KF2_SWAP=interval`, `KF2_SWAP=immediate` and `KF2_SWAP=vblank` are the
  comparisons. Whether Borderless is composed on a given driver, and so tear-free,
  is judged by eye. **No recompile.** See "VSync on Windows" in `docs/RUNTIME.md`.
  Since amended: off Windows, Borderless never covered the screen — Wayland lets
  no client position itself, and KWin fitted the undecorated X11 window to the
  work area (2560x1189 under the panel) — so Borderless is Windows-only in
  effect and takes GLFW's fullscreen elsewhere. It bought nothing there: the
  vblank wait is Windows-only, so VSync is the interval either way. The amendment is the second diff in
  the patch file.
  Since amended: on Windows, Borderless was the work area and not the monitor —
  Silk's `IMonitor.Bounds` is `glfwGetMonitorWorkarea` — so it stopped at the
  taskbar (1920x1128 on a 1920x1200 screen). It now takes `glfwGetMonitorPos` and
  the current video mode of that monitor, plus one row: a window exactly the
  monitor's size was promoted off the compositor and tore. The third diff in the
  patch file.

- `0067-screen-space-reflections.patch` — water reflects what is on screen above
  it, and the surface buffer and material id lighting will need. A blended triangle
  writes no depth, so at a water pixel the depth and `0058`'s normals are the pool's
  floor. `AoGeometry.V` gains a material and keeps a blended triangle that has one.
  `NormalFs` writes two outputs: the normal buffer, now blended `ONE,
  ONE_MINUS_SRC_ALPHA`, so a translucent surface leaves the opaque normal under it;
  and a new RGBA16F attachment on the target (`GlDisplayRt.Surface`), not blended,
  holding the last surface drawn at each pixel as an octahedral normal, a depth and
  a material. An edge-on opaque polygon writes a zero vector with alpha 1 instead
  of clearing, and `AoFs` treats a short vector as no normal. `Gpu/SurfaceMaterial.cs`
  is the id and its table (`Reflectivity`, `F0`); a triangle takes the packet's
  `GtePacketDepth.Rec.Material` (carried to `HleVertex.Material`), then a
  port-published VRAM rect's (translucent-only rects need blend mode 0 or 3), then
  `Opaque`. `GteDepth.Reflections` joins `DepthWanted` and `Active`, and
  `SurfacesWanted` (AO or reflections) replaces `AmbientOcclusion` at every site
  that meant "a pass wants the frame's surfaces": the far-plane mask, `zMode 4`,
  the opaque-texel depth draw and the projection read in `Gte.Rtp`. `SsrFs` marches
  the reflected ray through the depth with the GTE's H and centre, halves back to
  the crossing, falls back to the last far-plane pixel it crossed, and writes a
  premultiplied colour that `PresentFs` composites after the occlusion multiply.
  A hit's colour is fogged for its path through the mirror on the game's own depth
  cue (`Gte.Rtp` publishes DQA and DQB, `GteDepth.NoteDepthCue`), and the march
  runs to where that fog is black (`ScreenReflections.March`). `HleVertex.Projected`
  (the vertex map or PGXP answered) tells 2D from the scene; 2D triangles and
  sprites are kept as material `Overlay`, which the pass refuses as a sky sample or
  a hit, so the HUD is not reflected.
  Since amended: a floating object's reflection trailed down the water below it.
  The hit test accepted a sample behind a depth by less than the thickness plus the
  step's own run, which reaches about 1,350 units on the far steps, so a ray passing
  *behind* the gem over a pool counted as hitting it. A candidate is now halved back
  to its crossing and kept only if the ray is within the thickness of the surface
  there; otherwise the march goes on. The amendment is the second diff in the patch
  file.
  Since amended: `NormalFs` took a triangle's opacity from its material id
  (`vM < 1.5`), which was right only while every id above 1 was water. An authored
  id on an opaque floor would have dropped that floor out of the normal buffer. A
  blended triangle now carries its material plus `SurfaceMaterial.BlendedFlag` (128)
  in the surface list, and `NormalFs` takes opacity from that and the id from the
  rest; `SurfaceMaterial.FirstAuthored` (4) is where a port's ids start. Every
  existing id reaches the shader with the opacity it had, so the pass is unchanged.
  The amendment is the third diff in the patch file. See "Phase 1, the first slice"
  in `docs/REMASTER.md`.
  Since amended: `HleVertex.Projected` was set on any vertex-map hit, which was the
  same thing as "the GTE projected it" only while every published vertex came out
  of `Rtp`. The port now publishes the HUD's vertices itself, placed on the screen
  by `RotTrans` with no divide, so that they get their sub-pixel fraction; they are
  published with a depth of 0, and a hit counts as projected only with a depth. The
  HUD therefore stays 2D to the reflection pass (measured: its overlay count did not
  fall). The amendment is the fourth diff in the patch file. See "The HUD's
  transform in C#" in `docs/PATCHES_AND_MODS.md`.
  Since amended: the probe's readback keeps its per-pixel info texels
  (`ScreenReflections.LastInfo`, `LastW`, `LastH`, `MapSerial`), so the port can ask
  which material the GPU drew at a pixel. `patches/remaster/FaceProbe.cs` is the
  reader. The amendment is the fifth diff in the patch file. See "A tile half is a
  whole mesh, and a face is the key under it" in `docs/REMASTER.md`.
  Since amended: eight ids were four a port could author, so `SurfaceMaterial.Count`
  is 256 (the surface buffer's half-float alpha holds every integer to 2048, and the
  record holds a byte) and `BlendedFlag` 256. The table gains `Roughness` and
  `Emissive` (the latter `0071`'s, below) and a `Generation` a port bumps with
  `Changed()`; `GlCore` uploads it as a 256x2 RGBA32F texture on unit 6 when that
  moves, and `SsrFs` reads row 0 by `texelFetch` instead of two 8-float uniform
  arrays -- 256 of each would pass the fragment stage's uniform minimum. Roughness is
  a blur of the hit: nine taps over the footprint of the cone the reflected ray
  stands for, `roughness * distance` across at the hit's depth, the planar lookup
  taking its distance from the planar depth; 0 is the one read it was. With nothing
  authored the picture is the one before, to the bit (the pinned area-1 view's hash
  is Phase 2's `210d55698c875fb8`). The amendment is the sixth diff in the patch
  file, and it carries `0071`'s amendment too, since the two share `GlCore` and
  `GlShaders` hunks. See "Phase 3, the first slice" in `docs/REMASTER.md`.
  Since amended: the roughness blur's eight taps were turned per pixel by the 4x4
  interleaved pattern with nothing after it to cancel it, which on a busy texture
  left a woven grid repeating every 4 pixels of the pass (8 render pixels at the
  default resolution, 4 at full; measured, autocorrelation +0.39 to +0.72 at that
  lag, none at roughness 0). The pass now shrinks the picture, and the planar
  texture, into a half-size mip chain (`BuildMip`, units 7 and 8, only while an id is
  rough; `ScreenReflections.MipBuilds`) and reads the level whose texel spans the
  blur, centre and four taps, the same at every pixel: no repeat at any lag after.
  Roughness is squared before use. The table grows to 256x3: row 0's alpha is
  metalness, which tints a reflection with the surface's hue at full value; row 2
  is a highlight (`0071`'s) and the share of occlusion taken off, which the present
  shader reads from the surface buffer's id (`uAoMatOn`, only while an id is
  occluded other than fully). The seventh diff in the patch file, carrying `0071`'s
  amendment too. See "Phase 3, the second slice" in `docs/REMASTER.md`.
  Since amended: a metal is a tinted mirror. `SsrFs`'s outputs go through `emit()`,
  which takes half of the surface's own colour off at metalness 1, hit or miss, and
  the reflection's weight off what is left; `metalTint` pushes the hue a little
  from grey. With no metal the output is the one before, to the bit (the same
  hashes from the previous build). The reflectivity and F0 a metal gets are the
  port's (`patches/remaster/Surfaces.cs`). The eighth diff in the patch file. See
  "Metal is a tinted mirror" in `docs/REMASTER.md`.
  Since amended: the pass runs for each of its terms on its own. It was switched by
  the screen march, so `0068`'s planar texture and `0072`'s planes and cubemap were
  read only with SSR on. `ScreenReflections.Enabled` is the march alone now, and
  `GteDepth.Reflections` (the pass, the surface buffer, the materials) is on while
  the march, `PlanarReflections.Enabled`, `RetainedScene.Enabled` or the new
  `WaterMurk.Enabled` is (`ScreenReflections.Refresh`, called from each setter).
  `SsrFs` gains `uMarchOn`, which leaves a pixel no planar lookup answers
  unreflected, and the murk: a dark colour over water by the view ray's run from the
  surface to the depth buffer's floor under it, laid under the reflection in
  `emit()`. With the march on and no murk the output is the one before. The ninth
  diff in the patch file. See "The reflection pass runs for each term on its own"
  and "Murky water" in `docs/RENDERING.md`.
  `GlCore.RenderNormals` became `RenderSurfaces` and runs once for both passes,
  timed with the occlusion pass when that runs. New profiler sections (`Surfaces`,
  `Ssr`) and `GpuWork.Reflections`; the probe attaches a second target to the pass
  and reads back what each reflective pixel found (`ScreenReflections.SetMap`). The
  occlusion census is identical with it on and off. Off by default. GL core only.
  **No recompile.** See "Screen-space reflections" in `docs/RENDERING.md`.

- `0068-planar-reflections.patch` — the scene drawn a second time from the camera
  mirrored in the water, for the reflection pass to read before it marches.
  `Gpu/PlanarReflections.cs` is the interface: `Capturing` and `Serial`, which the
  port sets around its own `DrawOTag` of the mirrored table; `ClipPlane` (the
  water in the mirrored view) and `ViewPlane` (the same plane in the real view);
  and the plane finder. `GlCore.DrawTri` hands every triangle it classifies as
  water to `NoteWater`, which takes it back to world Y with the camera the port
  published (`SetCamera`), refuses one that is not level, and bins the area it
  covers on screen by height. `TakePlane` gives the port the heaviest band once
  a frame. While capturing, `Classify` swaps the target for its planar texture
  (`GlDisplayRt.Planar`, the same size and margin, `IsPlanar`, never written back
  to VRAM, linear-filtered, cleared on the first primitive of each capture, and
  carrying the two planes and the frame it was drawn in). A primitive with no
  target is dropped rather than drawn into VRAM, and a planar triangle is kept
  out of the surface list. `PrimFs` gains `uClipOn`/`uClipPlane`/`uClipCentre`/
  `uClipH` and discards a fragment on the camera's side of the plane, from the
  view position it rebuilds as `NormalFs` does. `SsrFs` gains `planarAt`: a
  surface within `Tolerance` of `ViewPlane` takes the texel at the mirrored row
  `2·OFY - y`, bent by the water's brightness gradient (`Ripple`), when the
  texel's depth or colour says something was drawn there; anything else marches
  as before. The probe's readback counts the planar outcome (5) and, on its own
  frame (`uCompare`), marches a planar pixel as well and writes both colours'
  brightness difference against the planar texture read unmirrored.
  `PlanarReflections.Supported` is set only by the GL core backend. Off by
  default. **No recompile.** See "Planar reflections" in `docs/RENDERING.md`.

- `0069-present-snap.patch` — a diagnostic: the presented picture read back once,
  on request. `Gpu/PresentSnap.cs` holds one pending request (skip `N` presents,
  then the first whose display buffer starts at a named VRAM row, or any after
  eight); `GlCore.PresentDisplay` asks it once per present, after the composite
  and any post shader, and `SnapPresent` reads the texture the window is drawn
  from with `glReadPixels` into RGBA8, top row first. VRAM could not answer this:
  the occlusion and reflection passes composite at present and write nothing back,
  so a VRAM hash cannot see what a material changes. One null test a present while
  nothing is asked. The port half is `patches/remaster/Snap.cs` (the `snap` shell
  verb). **No recompile.** See "Phase 1, the second slice" in `docs/REMASTER.md`.

- `0070-hook-order.patch` — `HookManager.AddPre` and `AddPost` take an `order`,
  and the hooks on one function run in ascending order, then in the order added.
  Without it the order was the order of the calls, so a post that must run after
  another's was kept there by where its patch's `Install()` sat in `Program.cs`:
  `LoopPacing`'s redraw on stage 13, after the smoothers' restores. The default is
  0 and an insert goes after every entry of the same or a lower order, so every
  existing order is unchanged. `Stage13.HookOrder` names the orders on the
  renderer. **No recompile.** See "The hooks on stage 13 are ordered by what they
  need" in `docs/PATCHES_AND_MODS.md`.

- `0071-authored-lights.patch` — the remaster's point and spot lights, as one more
  term in the lit colour. `Gpu/RemasterUniforms.cs` holds up to 16 lights the port
  has already put in the GTE's view space (position and radius, colour times
  intensity and the spot's inner cosine, direction and the outer cosine), with a
  generation. `PrimFs` gains `authored()`: the fragment's view position rebuilt
  from its recovered depth, H and the centre as `NormalFs` does, its normal from
  that position's screen derivatives (taken before any per-fragment test), and a
  smooth-windowed, cosine-weighted sum over the lights; `shade8` takes it and adds
  it times the packet's RGBC (the record's low bytes) to the lit colour before the
  depth cue, so fog, texture and saturation apply to it as to the game's light.
  Only a packet with a `0048` record and a depth is lit, and nothing is lit in a
  planar texture (`uClipOn`). `GlCore` uploads the arrays when the generation
  moves, sends the centre and H per batch like `uClipCentre`, and flushes a batch
  built under the previous generation (`FlushReason.StateLight`). With no light
  the shader's output is 0048's to the bit (`scripts/light_probe.c`).
  `patches/remaster/Lights.cs` is the only writer. GL core only. **No recompile.**
  See "Phase 2, the first slice" in `docs/REMASTER.md`.
  Since amended: a material can glow. `GlLight` carries the packet's material
  (attribute 11, beside the light record), and `PrimFs` adds
  `SurfaceMaterial.Emissive[id]`, row 1 of `0067`'s table, to the same term an
  authored light adds, before the depth cue -- so it is fogged and textured as the
  game's own light is, and times the packet's RGBC. It needs no depth, so unlike a
  light it is drawn into a planar texture too. `uEmitOn` is set only for a batch with
  records while some id glows; with it off, or with material 0, `light_probe.c`
  reads the shader as before to the bit. The hunks are in `0067`'s file (its sixth
  diff). See "Phase 3, the first slice" in `docs/REMASTER.md`.
  Since amended: the glow multiplied the texture, so glow 4 was the texture at twice
  full, clipped, and read as overexposed stone rather than a light source. Row 1's
  alpha now picks the mode (`SurfaceMaterial.EmissiveAdditive`): 0 is the old term,
  1 adds RGBC times the glow after the texture is modulated, fogged on the packet's
  own curve (`cueWeight()`, split out of `shade8`), on every output path. A packet
  without an additive glow adds zero, and `light_probe.c` reads the old passes as
  before and the four new ones at 0 from the formula. The amendment is the second
  diff in `0071`'s file. See "The glow is a light source" in `docs/REMASTER.md`.
  Since amended: an authored light leaves a highlight on an id with a specular
  value: normalised Blinn-Phong in `authored()`, its size from the id's roughness
  (squared, floored at 0.15), added past the texture and fogged, and tinted by the
  texel on a metal (`post()`). Row 1's alpha is flags now, 2 keeping an additive
  glow out of the fog. A point light's outer cosine below -2 names the material that
  gave it off (`-2 - id`), and that material is not lit by it. `light_probe.c` gains
  four passes (the highlight flat and on a textured metal, the skip, the unfogged
  glow), all at 0 from the formula, the first ten unchanged. The hunks are in
  `0067`'s seventh diff. See "Phase 3, the second slice" in `docs/REMASTER.md`.

- `0072-retained-scene.patch` — the area's geometry kept on the GPU in world
  space, so a reflection draws the world again without the game's walks.
  `Gpu/RetainedScene.cs` holds what the port fills: the static map as world-space
  corners (lit colour before the cue, CLUT, texpage, UV, the depth cue's DQA, DQB
  and curve, the texture rectangle and a material), sorted into five ranges
  (opaque, then each blend mode) and 8x8-tile chunks with their bounds; and a ring
  of four frames by serial, each the camera it was drawn with, its models and the
  planes it mirrors in. `GlDisplayRt.RetainedSerial` is the serial a target was
  drawn under, stamped in `FlushCore`. `GlShaders.WorldVs` gives `PrimFs` exactly
  what `PrimVs` does, from a world corner through a camera uniform, with W the
  view depth and a near plane, the corner fogged on its own curve at that camera's
  depth, or drawn at its mirror image in `Y = uPlaneY` with `gl_ClipDistance`
  removing what lies below. `PrimFs` gains `uMaskOn`: keep a fragment only where
  the presented frame's surface buffer lies within the tolerance of a plane; 0 is
  the shader as it was. `GlRetained.cs` (`GlCore` is now partial) draws at present,
  before the reflection pass and for the presented target's frame: every plane
  into the target's planar texture, unmirrored, and six faces of a cubemap with a
  depth cube, each view culling chunks by its frustum and the fog's reach. `SsrFs`
  gains `retPlanarAt` (the first plane the surface lies on, read unmirrored) and
  `cubeMarch` (the reflected ray in world axes against the depth cube, no jitter),
  which replaces the screen march while the cubemap is on; the probe's compare mode
  checks one against the other. The blend function is put back after the draws:
  dual-source factors left set make any draw into two buffers an error with
  blending off. The port half is `patches/RetainedMap.cs`, `RetainedPlanes.cs` and
  `RetainedModels.cs`. Off by default. GL core only. **No recompile.** See "The
  retained scene" in `docs/RENDERING.md`.
  Since amended: the mirror and the cube faces drew every face both ways, so the
  top of anything above the water showed under it seen from behind; they cull the
  faces the game culls now, clockwise being front through a mirror and a cube face
  (`RetainedScene.CullBack`). The distance cull went by straight-line distance and
  one fog, and dropped chunks at the picture's sides that kept up to 97% of their
  colour; it goes by view depth and each chunk's own latest fog now
  (`ChunkFogQ`). `WorldVs` fogs per pixel through `0048`'s `vFog`/`vLight`, and a
  surface on a plane whose planar texel is empty reflects nothing rather than
  marching the cubemap. Only the map halves the frame's own tile walk drew are
  reflected (`Frame.Halves`, `Flags` bits 13-26, `uHalves` in `WorldVs`; the
  shadow cubemaps are not gated), since the map holds both levels of every cell
  and the game's visibility flood draws far fewer. The probe counts the
  back-facing pixels, the undrawn halves' and the old cull's visible drops. The amendment is the second diff in the patch file. See "What the
  mirror showed that it should not, and the fog it dropped" in `docs/RENDERING.md`.

- `0073-texture-replacement-on-the-port-path.patch` — upstream's texture packs
  (`Assets/`) made to work in this port, and a way to see what they would key.
  **A draw with no display target marked VRAM GPU-dirty from boot.** `GlCore.V`
  took a batch's first vertex as `_count == 0`, but every caller writes
  `_verts[_count++] = V(...)`, which increments `_count` before `V` runs, so the
  batch bounds were never reset and grew to everything drawn since boot; the game's
  first untargeted draw (a 32x32 black box at `(0,344)`, clip 1024x1024) marked
  `(0,0)` 748x481 dirty, and the resolver refused every texture under it: 74 of
  area 1's 86 keys, which would never have been replaced. A flag now starts the
  bounds per batch; the dest-copy rectangle reads the same bounds and shrinks with
  it. **One piece of art had several keys**, because the key is the UV bounding box
  and this game's faces read a texel past their texture (`[191,63,65,64]` beside
  `[191,63,64,64]` for one texture); a clipped fan's triangles were keyed on their own
  UVs too. `VramTracker.NoteUpload` keeps which LoadImage last wrote each VRAM word,
  and `TextureResolver` keys a rectangle on that upload's when it lies inside it to
  within two texels (`KeyOnUpload`); `DrawTri` looks up by the face's `0060`
  rectangle (`KeyOnFaceRect`). Area 1: 109 keys by triangle, 86 by face, 21 by
  upload, and 471 overlapping pairs down to 6. **A replacement is filtered by the
  port's slider**: a mip chain at load, `LinearMipmapLinear` and the anisotropy
  level while mipmaps are on (`RepFilter`, set per texture when the slider moves),
  sampled by `textureGrad` on the unwrapped UV's gradients so a texture window's
  wrap is no seam; a replaced CLUT keeps the anisotropic taps and not the mip atlas
  (decoded through the game's CLUT). `TextureResolver.Observer` and
  `VramTracker.Uploaded` feed the port's census (`patches/remaster/TextureCensus.cs`);
  while an observer is set a lookup runs with no pack. With no pack the pinned
  area-1 view is `210d55698c875fb8`, as before. GL core only for the filter.
  **No recompile.** See "Phase 4, the first slice" in `docs/REMASTER.md`.
  Since amended: an image the game loads in pieces straight down, at one x and
  width (a 128x128 texture as 100 rows and 28), was two uploads, so a face on the
  second piece fell outside its upload and kept its own rectangle.
  `VramTracker.NoteUpload` extends the previous load when the next continues it;
  area 1's replacement keys went 21 to 17 and its overlapping rectangles 6 pairs
  to none. `TextureResolver.ToUpload` is public and returns whether it widened, so
  the port's texture materials key on the same rectangle. The amendment is the
  second diff in the patch file. See "Phase 4, the second slice" in
  `docs/REMASTER.md`.

- `0077-light-shadows.patch` — shadows for `0071`'s authored lights, from `0072`'s
  retained map. `RemasterUniforms` gains up to four shadow slots (the light's world
  position and radius), the slot each light in the list samples (`LightShadow`, -1
  none) and the frame's world-to-view rotation (`ToWorld`), which the port publishes
  with the lights; `RetainedScene.ShadowsWanted` asks for the static map without
  reflections. `GlShadows.cs` (`GlCore` partial) draws a slot's depth cubemap from the
  top of `FlushCore`, before the batch that samples it, and only when the light, the
  map's generation or the size changed: six faces through the world program
  (`WorldVs` and `PrimFs`, so a texel drawn as a hole casts none), the opaque range
  of the chunks the light's sphere reaches, into a `DepthComponent24` cubemap with a
  compare mode. What a face holds is what the world program already writes, the
  distance along the face's axis over 65536. `PrimFs` gains `uLightShadow[16]`,
  `uShadowToWorld`, four `samplerCubeShadow`s on units 12-15 (named, since GLSL 3.30
  cannot index a sampler array by a loop variable; a cube sampler left on unit 0 beside
  `uVram` fails every draw, so both programs set them at init) and `shadowAt()`: the
  fragment moved off its surface by `uShadowOffset` texels at its distance, and five
  compares, each the hardware's 2x2, at fixed offsets **along the surface** rather
  than across the face, so a sloped floor does not shadow itself. With no light
  shadowed the shader's output is `0071`'s to the bit (`scripts/light_probe.c`, whose
  first fourteen passes read as before; two new passes read 0 from the formula).
  `DrawRange` returns the static vertices it drew and takes no frame for a shadow.
  GL core only. **No recompile.** See "Shadows, the first slice" in
  `docs/REMASTER.md`.
  Since amended: models cast. A slot whose light has a model in reach samples a
  second cubemap, the map's blitted face by face and the frame's casters drawn over
  it, from the frame the port names in `RemasterUniforms.ShadowFrame`; it is drawn
  again when a hash of the casters in reach changes, and the map's cubemap still only
  when the light or the map does. Casters are the frame's opaque model triangles and
  those flagged `RetainedScene.FlagSolid` (a door), with every texel, then the other
  blended ones with `uOpaqueDepth = 1`, which keeps the texels the GPU draws opaque;
  `FlagNoShadow` (an effect) casts nothing. `RetainedScene.ShadowModels` is the
  switch; `ShadowModelRenders`, `ShadowModelTriangles` and `ShadowCasters` count it.
  The amendment is the second diff in the patch file. See "Shadows, the second
  slice" in `docs/REMASTER.md`.

- `0078-water-waves.patch` — ripples on water, per pixel. `Gpu/WaterWaves.cs` holds
  what the port publishes: the water's VRAM rects, the camera the frame was drawn
  with (`view = R (world - cam) + T`), a clock, and three settings (the push in world
  units, the longest ripple's length, the shading). `PrimFs` gains `uWave*`: a
  fragment whose texel lies in a rect is taken to world space from its depth, a
  four-wave field's slope becomes a push in the world, taken into texture space
  through the polygon's own mapping (the world position's and the UV's screen
  derivatives), and the pushed texel wraps inside the rect (`waveWrap`), the aniso
  taps with it; the slope also scales the texel. Faded out where a pixel spans too
  much of the field. `GlCore` sets `uWaveOn` for a batch in water's blend
  (semi-transparent, 0 or 3) and not into a planar texture, and sends the rest when
  `WaterWaves.Generation` moves. With `uWaveOn` 0 the texture path is the old one.
  The swell is the port's alone (`patches/WaterSwell.cs`). GL core only. **No
  recompile.** See "Water waves" in `docs/RENDERING.md`.

`0007`, `0008` and `patches/EndingHold.cs` are the shape to keep in mind
generally: **anything the runtime refreshes only at `VSync` is invisible to a
game that stops calling `VSync`**, and that failure mode is always silent.
`END.EXE` ends in `while(1);` with no `VSync`; on hardware the last frame stays
on the CRT, here the window dies. **Holding it is faithful and still reads as a
crash**: the spin is real (`08004694 00000000` at `0x80011A50` in the disc image)
and `END.EXE` never writes the boot stub's next-executable byte, so on hardware
the ending is a hang you leave with the reset button — and a window has no reset
button, which is why "the game crashes after The End" was reported again after
the hold was already in. Measured holding, alive and pumping, at 20 fps through
`GAME.EXE`'s own hand-over and at 144 fps. So **any button now leaves the still
for the title** (`KF2_ENDINGEXIT=0` keeps the hang), through the stub's own
loader: `SLUS_001.58` holds three file names at `0x80010254`
(`0` = `OPEN.EXE`, `1` = `GAME.EXE`, `2` = `END.EXE`), an index at `0x80010268`
and a loop in `func_80010038` that `Load`s, `Exec`s *as a call* and re-reads the
index from `0x800102F0` on return — which is exactly the door `GAME.EXE`'s own
quit-to-title uses. `patches/BootExe.cs` (`KF2_BOOTEXE`) writes that same index
before the loop's first pass, **once**, so the ending is reachable in seconds
rather than by finishing the game. See "The ending screen" in `docs/RUNTIME.md`.
