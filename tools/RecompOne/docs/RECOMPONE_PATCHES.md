# The port's changes to RecompOne, one by one

This record moved here from Verdite2 (`Voicedrew11/verdite2`) on 2026-10-02, so
that every game taking this fork has the record its source cites. In a game
repository it is `tools/RecompOne/docs/RECOMPONE_PATCHES.md`, and the diffs are
`tools/RecompOne/patches/`. Every other `docs/` file named below (`RUNTIME.md`,
`RENDERING.md`, `REMASTER.md` and the rest) is Verdite2's, where the work was done.

`patches/*.patch` are no longer replayed (the tree is this fork, taken into each
game as a subtree — see Verdite2's `docs/RECOMPONE_FORK.md`), but they are still the record of what the port changed
in the runtime and the recompiler and why, and the numbering is still how each
change is referred to in the source. `docs/RUNTIME.md`'s "The patches to the
checkout, one by one" covers the early ones at more length; this list is the
complete one.

**A new number is for a new mechanism.** A correction to an existing patch's own
logic amends that patch instead: a "Since amended:" paragraph in its entry, the
amendment's diff appended to its `.patch` file, and the source comments keep its
number. `0016` and `0057` predate this and keep their numbers, since the source
refers to them.

Seventy-two of the eighty are load-bearing; `0002`, `0003`, `0015`, `0045`,
`0046`, `0065`, `0069` and `0084` are diagnostics and `0013` is a settings-placement hook. `0063` is retired:
it was folded into `0054` as an amendment, and the number is not reused. `0075` and `0076`
are held by the remaster's plan for work not yet made (`docs/REMASTER.md`), which is
why the shadows are `0077`. **Three force a recompile** —
`0004`, `0035` and `0037`; every other one changes runtime behaviour only. **One
patch has an asset beside it**: `patches/assets/` holds the TTF `0033`
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
  **Amended 2026-10-02: the vblank root counter's event (`0xF2000003`,
  `EvSpINT`) is delivered once a vblank, by IRQ 0's service, and no longer
  directly from `LibEtc.TickVBlank` as well.** Both deliveries ran, so every
  handler on that event ran twice a vblank on this timeline (upstream's blocking
  one raises IRQ 0 only). Measured 120.0 → 60.0 a second in both games: Verdite2's
  `func_80017850` clock `0x801B6CAC`, Verdite3's `func_80019570` count
  `0x801C12E8`. Verdite3's own frame gate then holds it to 15 frames, not 30;
  Verdite2's acceptance numbers are unchanged. **No recompile.**
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
  Since amended: `OutputView.DockId`, the dock node the Output panel sits in (0
  while it floats). A node holding a window is a leaf, so the port can split it
  with `DockBuilder` to dock a panel of its own beside the picture: the remaster
  editor opens at the right edge that way. The third diff in the patch file. See
  "Modes" in `docs/REMASTER.md`.

- `0030-expose-host-pump.patch` — the shipped launcher has to build the game
  before there is a game to run, and that blocks for seconds; a window that stops
  pumping for seconds is one the desktop offers to force-quit. `HostWindow.Pump`
  already does exactly the right thing and is `internal`, and `WaitForValidDisc`
  already runs that loop but only ever for its own condition. Exposed as
  `Runtime.Pump`. Everything else the progress UI needs was public already —
  `Popup` is abstract-public and `PopupManager.Register` takes any implementation
  — so the launcher's `BuildProgressPopup.cs` (Verdite Core's `launcher/`, shared
  by the ports since 2026-10-05) is not a patch. UI only, **no
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
  (`patches/assets/NotoSans-OFL.txt`, which the packaging must ship).
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
  Since amended: the BK and LCM were kept in a ring of eight generations, and a
  generation starts whenever the constants change. Every model sets its own from its
  tile's light record and the planar walk replays them, so area 7 starts up to 11 in
  a frame, and when the ring wrapped before the table was drawn, the first models'
  packets uploaded a later model's BK: darker by a constant, 22-33 levels. The ring is
  64 now, and `GteLightMap.Generations` counts them for `0085`'s probe. The amendment
  is the second diff in the patch file. See "Step 3, the first slice" in
  `docs/GPU_RENDERER.md`.

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
  *Amended (2026-10-05):* a kept triangle carries its UV and texel word
  (`GlCore.SurfaceTex`, `VeilTex`'s packing), and `NormalFs` drops a texel that is
  `0x0000` in sample VRAM, at `PrimFs`'s centre tap, as the colour pass does. Before,
  a billboard's transparent texels wrote no depth but did write its camera-facing
  normal, so AO shaded the wall behind at that normal: a faint box round every
  sprite with AO on. A texture window or an image keeps the whole face (the decode
  has no window). The normal pass now always binds sample VRAM on unit 0.

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
  both is each game's `patches/CardIcon.cs` and Verdite Core's `WindowIcon` and
  `DesktopEntry`. UI only — **no
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
  Since amended: an opaque triangle drawn in painter's order (zMode 3, no depth
  record) was kept out of the surface list unless no corner was projected, so the
  first-person arm, whose corners the GTE projects but whose packets are not
  recorded, left the water under it in the surface buffer, and the murk and the
  reflection were composited over the arm. It is kept as `Overlay` now, except in
  the table's slot 0 (the skybox, which must read as no surface). The tenth diff in
  the patch file. See "The arm showed the water through it" in `docs/RENDERING.md`.
  Since amended: the one-texel crack fill took a pillar in front of the water as
  the floor, leaving a strip unmurked; it takes only a depth behind the water now.
  And `PresentFs` upsamples the pass by the surface
  under each pixel (`ssrAt`), with the surface buffer at the render scale while the
  pass runs and the depth on unit 4. The eleventh diff in the patch file. See "A
  halo round the pier's pillars" in `docs/RENDERING.md`.
  Since amended: a see-through 2D primitive (a name box, the HUD panel) is a veil
  in the surface list rather than an `Overlay`: it keeps the water under it and adds
  512 (blend mode 0) or 1024 to the id, and the pass murks and reflects that water
  at the share the box lets through. A textured one is decided per texel from
  sample VRAM in `NormalFs`, its opaque texels an `Overlay` as before. The twelfth
  diff in the patch file. See "A see-through box showed the water unmurked" in
  `docs/RENDERING.md`.
  Since amended: only a level surface is murked. `uMurkUp` carries the world's vertical in view
  space (`WaterMurk.UpX/Y/Z`, which the port publishes) and the cosine a murked surface may lean
  to (`WaterMurk.MaxTilt`, 0.75); a crystal in the water's texture, with nothing behind it, had
  taken the sky's endless run and gone to the murk's colour. The thirteenth diff in the patch
  file. See "Only level water is murked" in `docs/RENDERING.md`.
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
  Since amended: `RestHeight`, where the port says water rests at a world X and Z.
  The swell moves the water's vertices, so its triangles were refused as not level
  and the plane was the mean of the few left, moving on 143 of 144 frames.
  `NoteWater` takes a triangle's centroid to world X and Z (`SetCamera` gains the
  camera's X and Z) and bins an answered triangle at the rest height with no level
  test; `WaterRested` counts them. With no answer it is the old path. The
  amendment is the second diff in the patch file. See "The swell moved the water
  off the mirror" in `docs/RENDERING.md`.
  Since amended: a capture fogs a fragment at the larger of its own view depth and
  its depth along the view's level forward (`PlanarReflections.LevelAxis`,
  `uClipLevel`, `uClipDq`), rescaling the recorded depth cue's part past DQB. The
  game culls its map by a level cone and fogs by view depth, so the cone's far edge
  reflected lit from a mirrored camera looking up, and popped in in the water. Level,
  the capture is unchanged. The probe reads the planar texture back by that depth
  (`FogCensus`). `KF2_PLANAR_FOG=0` is the comparison. The third diff in the patch
  file. See "The fog the mirror dropped" in `docs/RENDERING.md`.
  Since amended: on its plane the planar answer is final. A surface on the plane
  whose planar texel is empty reflects the background (`uAtmosSky`, or black) at
  the water's weight instead of marching, so a cell culled in or out of the mirror
  no longer flips a pixel between the march and the mirror (`gPlanarEmpty`,
  `onPlanar`), and `0083`'s enhancement distance leaves a surface on the plane
  alone. The fourth diff in the patch file. See "The planar walk is the
  reflection, with a cull of its own" in `docs/RENDERING.md`.

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
  Since amended: `0071`'s authored lights and glows reach the world program. A
  corner carries the RGBC the game lit it from (`Vertex.Rgbc`, attribute 8; 0 leaves
  it out), `RemasterUniforms.LightWorldPos`/`LightWorldDir` hold the lights in world
  space, and `SendWorldLights` turns them into each mirror's and cube face's view
  (mirrored first for a plane), with the projection `authored()` rebuilds a fragment
  with and the shadow lookup's view-to-world turn. And the mip atlas (`0060`): an
  entry per distinct static texture, looked up every present so it stays resident,
  in a buffer of its own (attribute 9) re-uploaded only when an entry moves; the
  frame's models' per present. `RetainedScene.Lit` and `Mips` are the comparisons.
  The third diff in the patch file. See "Lights, fog blends and mipmaps in the
  reflections" in `docs/RENDERING.md`.
  Since amended: a half's gate byte is a weight. `NoteHalf` writes 255, and
  `RetainedScene.CurrentHalves` lets the port write its own, 0 to 255, after the
  frame's walk. `WorldVs` passes it as `vFade` (1 for anything the gate does not
  weigh, and for the shadow and probe draws), `PrimVs` writes 1, and `PrimFs` drops a
  fragment whose 4x4 ordered-dither threshold is above it. With every weight 255 the
  shader's output is the one before (`scripts/light_probe.c` and `shader_probe.c`
  unchanged). The fourth diff in the patch file; the discard shares a hunk with
  `0083` and is in that file. See "The reflections see past the camera's cull" in
  `docs/RENDERING.md`.
  Since amended: a static corner carried its mip-atlas entry, and every texture on
  the map was looked up each present (1,633 in `fdat02`, 361 of them held), which
  re-uploaded the map's whole entry buffer whenever one moved and kept textures no
  frame draws in the atlas. A corner now carries its texture's index plus one;
  `WorldVs` reads the entry from a table of one word per texture (`uMipTable`, a
  buffer texture on unit 17, `uMipIndirect` 1 for the static map and 0 for the frame's
  models); and `UpdateWorldMips` looks up only the textures of the halves a draw
  gates in (`Frame.Halves` while the half gate is on). The hunks are in `0085`'s
  file. See "Step 1, the first slice" in `docs/GPU_RENDERER.md`.

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
  Since amended: a texture the game scrolls (`func_8002DC78` rewrites its VRAM at a
  new phase every tick, so no hash of the VRAM holds) is replaced by its source
  image's replacement. `TextureResolver.Scroll` asks the port for the source's key,
  the dest rectangle and the phase to draw at; `ResolvedTexture.Scrolls`/`Scroll`
  carry them, `GlCore` batches on the phase (`uRepScroll`, -1 for none) and sets the
  texture to wrap in V, and both prim shaders read row `d` of the dest at the
  replacement's `(d - phase) mod h`. `TextureDumper.OfferImage` dumps an image that
  is not in VRAM as it is keyed, so the source can be dumped at all. With no pack
  and nothing observing, `Scroll` is never called. The third diff in the patch file.
  See "Phase 8, the second slice" in `docs/REMASTER.md`.

- `0074-fog-colour-and-sky.patch` — the area's fog colour and curve, from the
  remaster. The game's depth cue darkens a colour towards the GTE's far colour,
  which is 0, so everything fades to black; putting a colour in the far colour
  itself would tint the *vertex* colour, which then multiplies the texture, and a
  distant wall would come out texture-times-fog rather than fog. So the colour is
  added **past the texture**: `shade8` darkens the lit colour by the cue's weight as
  before and keeps `uAtmosColour` times the same weight in `gFog8`, which every
  output path adds after the texel is modulated (`fogAdd`), so the result is a mix
  towards the colour. A blended texel whose batch adds or subtracts skips it
  (`uAtmosSkip`, set from the batch's blend mode), since fog takes such a texel
  away rather than to a colour. `uAtmosShape` bends the weight after the game's
  curve: raised to `x` and capped at `y`, in `cueWeight()`, so the glow and the
  highlight fog on the same curve. `RemasterUniforms` gains the fog block
  (`FogOn`, `FogColour`, `FogPower`, `FogMax`, `SkyColour`, `PublishFog`, which
  also bumps `Generation` so a batch is drawn under the fog it was built with). The
  world program sends it for the retained planes (`SendWorldAtmos`, the blend
  ranges setting the skip), and `SsrFs` fogs a reflection towards the colour on the
  same curve (`fogTo`, `refog`) and reflects the sky on a cubemap miss. Only a
  packet with a `0048` record takes it; the rest keep the game's black. With the
  switch off, and with it on at black and the game's curve, the shader's output is
  `0071`/`0077`'s to the bit (`scripts/light_probe.c`, five new passes, all 0 from
  the formula). The sky is the port's alone: `patches/remaster/Atmosphere.cs`
  writes it into the `DRAWENV` the game's background clear reads. GL core only.
  **No recompile.** See "Phase 5, the second slice" in `docs/REMASTER.md`.

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

- `0079-translucent-after-opaque.patch` — a blended polygon the depth buffer tests
  is drawn after the opaque tested polygons the ordering table put after it. It
  writes no depth, so those painted over it wherever they passed the test: a fish
  under the water drawn on top of it, a floor tile a triangle of water missing.
  `LibGpu.WalkOTag` is `DrawOTag`'s walk, public, with a callback told each
  packet's entry before it is sent (`patches/Widescreen.cs` now calls it instead of
  a copy). `Gpu/BlendOrder.cs` classifies a packet by its command and its
  `0050` record (`GtePacketDepth.Peek`, `Find` without the counters): a blended
  polygon with a full record and not solid is held; an opaque one with a full record
  is sent past the held ones; anything else sends them first, in table order, each
  under its own `OtEntry` and `OtSlot`. Only while `GteDepth.ZBuffer` and
  `GtePacketDepth.Active`, and not with an asset pack's own primitives. The probe
  (`BlendOrder.Probe`) samples what is sent on a 4-pixel grid and counts opaque
  samples behind a nearer translucent one; `Rec.Model` splits them by source. **No
  recompile.** See "Water was painted over by what lay under it" in
  `docs/RENDERING.md`.

- `0080-imgui-size-after-fullscreen.patch` — Silk's `ImGuiController` takes the
  window's size only from the `Resize` event, and GLFW on Wayland raises none when
  a window leaves fullscreen (the framebuffer callback fires; the window-size one
  does not), so `io.DisplaySize` stayed at the fullscreen size and the menu bar
  lay above the window. `HostWindow.SyncImGuiSize` compares `io.DisplaySize` with
  `IWindow.Size` before each `Update()` and hands the controller's private
  `WindowResized` the real size on a difference. UI only — **no recompile**. See
  "Leaving fullscreen left the interface at the fullscreen size" in
  `docs/RUNTIME.md`.

- `0081-menu-bar-right-items.patch` — the right end of the menu bar held only the
  FPS counter: `MainMenuBar.Draw` set `MenuRegistry.RightAligned` to it every
  frame, so anything else a port put there was overwritten. `AddRightItem(width,
  draw)` adds an item there. Items are laid out right to left from the edge, the
  counter being the first of them, and a width of 0 skips one (the counter's is 0
  when it is off). The counter's slot is never narrower than `000 / 000 fps`, so
  what sits left of it does not move as the count changes width.
  The launcher's `UpdateBadge.cs` (Verdite Core's `launcher/`) is the only caller. Numbered past
  `remaster-design`'s `0070`-`0080` so the two branches do not collide. UI only —
  **no recompile**. See "Telling the player about a new release" in
  `docs/PACKAGING.md`.

- `0082-expose-input-pump.patch` — `HostWindow.PumpInput` becomes public, so the
  port can take in host events at the moment it samples the mouse. Motion was
  pumped at the present, before the pacing wait, and by the pad read on a tick
  frame, after it, so a tick frame's view carried most of a wait's extra motion
  and the frame after it almost none: at 60 fps a steady turn drew 22, 1 and 11
  units a tick. `Mouse.Poll` is the only caller. Input only — **no recompile**.
  See "The mouse was sampled at two points" in `docs/INPUT.md`.

- `0083-enhancement-distance.patch` — past a view depth, a surface is drawn the
  game's own way. `GteDepth.PlainDepth` (0, the default, is off) is sent as
  `uPlainZ` to `PrimFs`, `AoFs` and `SsrFs`, and each fades its own additions out
  over the 2048 units before it, by the recovered depth: `PrimFs` mixes the per-pixel
  lit colour towards the packet's corner colour (keeping a lit-mode glow), scales
  the authored lights and their highlight down, mixes the filtered texel towards the
  centre one, and scales the ripple's slope; `AoFs` fades the occlusion to 1; `SsrFs`
  scales the whole output by `gShare`. A fragment with no recovered depth is never
  cut. `GteDepth.PlainDepthLive` says the prim program has the uniform.
  `scripts/light_probe.c` and `scripts/shader_probe.c` read the old passes the same
  as the shader at `HEAD` and the new ones at 0 from the formula. The port half is
  `patches/EnhancementDistance.cs`. GL core only. **No recompile.** See "The
  enhancement distance" in `docs/RENDERING.md`.

- `0084-gpu-frame-timers.patch` — a diagnostic: GPU time per present, by pass.
  `Diagnostics/GpuTimes.cs` holds the totals; while `GpuTimes.Enabled` (the port
  sets it with the profiler) `GlCore`'s `BeginGpuTimer`/`EndGpuTimer`, which `0046`
  used only under a trace sink, queue each query with its pass (a flush into a
  planar texture is `Capture`, any other `Scene`; then AO, reflections, composite)
  and the present it was issued in, and `ResolveGpuTimes` reads them back at the
  end of each present without waiting. `GpuTimes.Issued`, `Complete` and the
  `Resolved` event let the port charge each query to the frame that issued it
  (`patches/GpuFrames.cs`). A trace sink takes precedence, and the
  retained scene's probe timer stands down, since `GL_TIME_ELAPSED` queries may
  not nest. **No recompile.** See "GPU time per present" in `docs/DEVELOPMENT.md`.

- `0085-gpu-world-main-view.patch` — the retained map (`0072`) drawn into the frame
  itself, the first slice of the GPU world renderer. `RetainedScene.MainView`,
  `MainSerial` (the frame whose map the next table walk draws) and `MainDrawer`, which
  `GlCore` fills; `LibGpu.WalkOTag` calls `Gpu.DrawRetainedMain` as the walk reaches
  slot 1, past the sky, and not in a planar capture or an asset pack's custom order.
  `GlMainView.cs` flushes the batch, takes the display target the draw area names,
  clears its depth as `FlushCore` would on a first draw (`ClearStaleDepth`, now shared),
  and draws the static opaque range through `WorldVs` with the frame's camera, centred
  by the GPU's draw offset and the margin, scissored to the game's clip, culled on
  facing, depth tested and written, gated to `Frame.MainHalves` (the halves the walk
  visited, which `ReflectionReach` does not grow). `GpuTimes` gains the `World` pass.
  The first cut spent 1.3-1.5 ms a frame keeping mip-atlas entries current, so `0072`
  is amended with it (below). `WorldVs` gains `uWorldSnap`, `uWorldPerPixel` and
  `uWorldDither`, so the main view follows sub-pixel, per-pixel lighting and the
  crosshatch as the frame's packets do; the reflections leave them at their defaults.
  Against the packet path, at most 4.9% of pixels differ by more than 4 levels in
  `fdat02` and 14.2% in area 1, all texel edges one render pixel over. GL core only. **No
  recompile.** See "Step 1, the first slice" in `docs/GPU_RENDERER.md`.
  Since amended: the map drawn on the GPU reached neither the occlusion pass's
  normal buffer nor the surface buffer (`0058`, `0067`), so the table's triangles
  alone filled them, in table order, and a creature behind a wall left its normal
  where the wall stood: its shading showed through. `RenderSurfaces` now draws the
  frame's map first (`GlMainView.DrawWorldNormals`, `WorldNormalVs` with `NormalFs`,
  for the frame `AoGeometry.WorldSerial` names, through its half gate), and
  `NormalFs` drops a fragment behind the frame's own depth (`uDepthCull`,
  `uFrameDepth` on unit 18), since the order no longer says which surface is in
  front. Without the map on the GPU the uniform is 0 and the pass is the one before.
  `RetainedScene.MainSurfaces` (`KF2_GPUWORLD_SURFACES=0`, `gpuworld surfaces off`)
  is the comparison, and `RetainedScene.SurfaceCheck` the probe's readback. The
  second diff in the patch file. See "Step 2, the first slice" in
  `docs/GPU_RENDERER.md`.
  Since amended: the main view's per-pixel fog was the corners' raw depth cue
  interpolated flat across the screen (`vFog`), which holds only while every corner
  is in front of the eye. The game's clipper hands the GPU only such corners; the
  GPU's own clipper does not, so a floor face clipped at the camera's feet took a
  wrong value along the clip and fogged to black in the corner of the picture.
  `WorldVs` now passes each corner's DQA and DQB perspective-correct (`vCue`), and
  `PrimFs`'s `fogRaw()` takes `DQA · H/z + DQB` at the pixel's own depth while
  `uCueFromZ` is set (the main view only, `RetainedScene.MainFogFromZ`,
  `KF2_GPUWORLD_FOGZ=0` to compare); for a face with one cue that is the
  screen-affine value exactly. The main view's near plane is
  `RetainedScene.MainNear` (`KF2_GPUWORLD_NEAR`, 16), which the measurement ruled
  out. The third diff in the patch file. See "Step 2, the second slice" in
  `docs/GPU_RENDERER.md`.
  Since amended: the map's blended faces (water) are drawn by the backend too.
  `GlMainView.SortWater` sorts the frame's visible ones far to near per blend mode by
  the key the game links a face at, and `LibGpu.WalkOTag` hands the backend each
  point where the walk sends a packet that could cover them: before each of 0079's
  held packets (`SendHeld`, with the packet's screen box, `PacketBox`), before each
  barrier that draws, and at the walk's end (`Gpu.DrawRetainedWater`,
  `RetainedScene.WaterDrawer`). The backend draws whole faces whose key the walk has
  passed, unless none shares a screen box with that packet, in which case they wait.
  `WorldVs` and `WorldNormalVs` move corners flagged `RetainedScene.FlagSwell` by the
  frame's three swell waves (`uSwell`, `RetainedScene.SetSwell`); `FlagWater` marks
  a face the surface buffer takes as water, and `FlagQuadTail` a quad's second
  triangle. The normal pass draws the water per pixel at the same cuts
  (`AoGeometry.Water`, `uZSlice` in `NormalFs`), and the plane finder is fed from it.
  The world program's uniforms are set once a frame and kept between the slices
  (`BindWorldMain`, `CloseWorldMain` before a shadow or reflection draw). With
  `RetainedScene.MainWater` off, the water stays on the packets as before. The fourth
  diff in the patch file. See "Step 2, the third slice" in `docs/GPU_RENDERER.md`.
  Since amended: the object walk's opaque models are drawn by the backend too, after
  the map. The port adds each model's opaque faces to the frame
  (`RetainedScene.AddMainModel`, `Frame.Models`), in runs lit by one BK and LCM
  (`ModelGroup`); a gouraud corner carries its normal's three light dots
  (`FlagDots`), which `WorldVs` lights with `uLightBk` and `uLcmR/G/B` as `PrimFs`
  lights a directional record, and a run may ask for the facing cull (`Cull`, faces
  the port could not cull as the game does). `GlMainView.DrawWorldModels` draws them
  after the map with 0051's depth prepass and bias, uncapped by chunks or the half
  gate; the normal pass draws them after the map. Each frame's models are uploaded
  once, to a buffer in a ring of four keyed by serial, with their mip-atlas entries,
  so the normal pass at present finds the presented frame's. `MainModelsShown` off
  leaves them undrawn, the probe's measure of what they cover. The fifth diff in the
  patch file. See "Step 3, the first slice" in `docs/GPU_RENDERER.md`.
  Since amended: the planar walk's mirror is drawn by the backend too. A frame
  carries a mirror (`RetainedScene.BeginMirror`: the mirrored camera, its own half
  gate `Frame.MirrorHalves`, `NoteMirrorHalf`, and its models, `AddMainModel`'s
  `mirror`; a frame's model runs are `ModelRuns` now), and `LibGpu.WalkOTag` calls the
  same drawer as a planar capture reaches slot 1 (`MirrorSerial`).
  `GlMainView.DrawWorldMirror` draws the map's opaque range and the mirror's models
  into the capture's planar texture through `WorldVs` from the mirrored camera, with
  PrimFs's clip plane and level fog (`uClipOn` and the rest, as `GlCore` sends them for
  a planar batch), then its blended faces whole, far to near, unswollen and unrippled
  (`DrawMirrorWater`, `RetainedScene.MirrorWater`); `SortWater` takes the view and the
  gate. `MirrorShown` off leaves it undrawn, the probe's measure of what it covers.
  The sixth diff in the patch file. See "Step 5, the first slice" in
  `docs/GPU_RENDERER.md`.
  Since amended: models drawn from meshes kept on the GPU. `RetainedScene.MeshCorners`
  holds each mesh's opaque faces in the model's own space (a corner's vertex index,
  normal and its face's four vertex indices in `Vertex`'s fields), appended to and
  emptied by the port (`AddMesh`, `ClearMeshes`, `MeshGeneration`); a frame carries
  its posed vertices (`AddModelVertices`) and a `ModelInstance` per model, in the main
  view and the mirror (`AddInstance`; `Mirrored` ones are copied at `BeginMirror`).
  `GlModelMeshes.cs` uploads the store as it grows, the frame's vertices into an
  RGBA16I buffer texture per frame of the ring (unit 19), and a table of the meshes'
  atlas entries bound on unit 17 while they draw, and draws each instance after the
  map (0051's prepass and bias) and into the normal pass. `ModelGlsl`, in `WorldVs`
  and `WorldNormalVs` behind `uModel`: a corner fetches its vertex and its face's,
  places them with the instance's rotation and translation, drops the face as the lit
  assembler does (mean table depth, facing on the GTE's saturated projection,
  `uModelGteC`; a negative bias wraps the test, `uModelNear`), and lights its normal to dots with the instance's LLM. With `uModel`
  0 the programs are the ones before. The seventh diff in the patch file. See
  "Step 3, the second slice" in `docs/GPU_RENDERER.md`.
  Since amended: the vertices are kept on the GPU too. `RetainedScene.PoseStore`
  (`AddPose`, `PoseTexels`, emptied by `ClearMeshes`) holds a rigid model's vertices,
  a texel each, and an MO pose's keyframe and its delta to the segment's target, two
  texels a vertex; an instance names its first texel (`Pose`), and for a pose its
  weight (`PoseMorph`, `PoseWeight`). `GlModelMeshes` uploads the store as it grows
  into an RGBA16I buffer texture on unit 20, and `ModelGlsl`'s `modelPosed` blends a
  pose as the game's decoder does, `key + (short)((delta * weight) >> 12)` in 16 bits.
  An instance with no `Pose` reads the frame's vertices as before. The eighth diff in
  the patch file. See "Step 3, the third slice" in `docs/GPU_RENDERER.md`.
  Since amended: the first-person arm is drawn by the backend too, in painter's order,
  as its unrecorded packets were. The port hands the frame the arm's instance and its
  faces' corners in the walk's order, in runs of one key, with the screen box it covers
  (`RetainedScene.SetArm`); `LibGpu.WalkOTag` calls `Gpu.DrawRetainedArm` before the
  first packet past a run's slot that draws and meets that box (`ArmCut`, `ArmMeets`),
  after the held packets and the water walked before it, as for a barrier; and
  `GlModelMeshes.DrawWorldArm` draws the runs passed with the depth test off and
  `PrimFs`'s `uFarPlane` writing the far plane, as zMode 3 does. The normal pass draws
  it as an `Overlay` at its place in the list (`AoGeometry.ArmAt`). `ModelGlsl` gains
  `modelPlace`, which puts a model's corner nearer than H/2, or past the screen clamp,
  where the GTE's saturated projection puts it, with no near clip, and `modelEye`, which
  places a `ModelInstance.ViewSpace` instance with the GTE's own matrix in integers.
  With `uFarPlane` 0 the shader is the one before. The ninth diff in the patch file.
  See "Step 3, the fourth slice" in `docs/GPU_RENDERER.md`.
  Since amended: the static map is lit and fogged in `WorldVs` from the area's 64
  light records, so a record the game rewrites is an upload and not a rebuild.
  `RetainedScene.Records` (52 ints a record: the light matrix at each quarter turn,
  the colour matrix, the back colour, the fog word, its DQA and DQB and its curve;
  `SetRecords`, `RecordGeneration`) is uploaded as a 13x64 RGBA32I texture on unit 21
  (`GlRetained.UploadRecords`, from `UploadStatic`). A corner whose
  `Vertex.Light` (attribute 10) has bit 31 carries its face's normal in R, G and B,
  its two EvenFog weights in DQA and DQB, and the records of its half and the three
  it blends with; `recordLit` lights it as `NormalColorCol` does and blends the colour
  matrix, back colour and fog as `EvenFog` does, in the same integers (`mixExact`
  rounds half away from zero as the CPU's double does). A chunk's fog bound
  (`ChunkFogQ`) is taken again from its records when they change. With `Light` 0 the
  program is the one before. The tenth diff in the patch file. See "Step 1, the second
  slice" in `docs/GPU_RENDERER.md`.
  Since amended: a model's blended faces are drawn by the backend too. A mesh keeps its
  blended faces' corners after its opaque ones, with a table of where each face's corners
  are (`RetainedModels.Mesh.FaceAt`); the port notes each blended face of a main-view
  instance with the table slot the lit assembler would link it at, taken from the GTE's own
  matrix in integers (`RetainedScene.AddBlendFace`, `Frame.BlendFaces`), so no transform
  and no assembler run. `GlMainView.SortBlend` sorts them by blend mode, slot and build
  order into an element buffer on the mesh VAO, and `DrawWorldWater` merges them with the
  map's water by key (a model first at one key, since it was built after the map), drawing
  a run per instance through `SendInstance`, and their opaque texels' depth after. A
  `ModelInstance` gains `MeshAll` and `Solid`. The forced-blend twin (effects, billboards)
  takes the same route. Subtractive faces stay on the packets. The eleventh diff in the
  patch file. See "Step 3, the fifth slice" in `docs/GPU_RENDERER.md`.
  Since amended: the sky, subtractive faces, and every blend mode in one order. The objects of
  kind 0xF0 go to the frame's sky (`RetainedScene.AddSky`, `Frame.Sky`, `SkyFaces`), instances
  with `ModelInstance.Sky`, which `ModelGlsl` keeps by facing alone on whole pixels and `WorldVs`
  lights per corner with no cue and no authored light (`uModelSky`); an untextured face carries
  its own colour in `Vertex.Light` (`RetainedScene.FaceColour`). `GlModelMeshes.DrawSky` draws
  them as the main view begins, before the map, far key first and the last linked first, untested,
  an opaque face writing the far plane. `GlModelMeshes.DrawBlended` blends a draw at the console's
  rate, mode 2 in GlCore's two passes, and every blended draw of the main view and the mirror goes
  through it, so range 3 is drawn with the rest. `DrawWorldWater` merges the water's four ranges
  and the models' four modes into the table's order instead of drawing one mode after another
  (`RetainedScene.MainWaterRuns` counts the runs). The twelfth diff in the patch file. See "Step 3,
  the sixth slice" in `docs/GPU_RENDERER.md`.
  Since amended: the objects near the camera, the mirror's blended faces, and blended faces in the
  surface buffer. A `ModelInstance.Tile` is assembled as `func_80030540` assembles it: no depth range,
  a face whose corners project without saturating kept by its facing on the screen (a quad on its
  whole loop), any other by its plane against the eye and left to the GPU's near clip, and no corner
  at the saturated projection (`uModelTile`, `modelTileKept`, `modelProjects` in `ModelGlsl`). A frame
  carries the mirror's blended faces (`Frame.MirrorBlendFaces`, `AddBlendFace`'s `mirror`), which
  `DrawMirrorWater` merges with the mirror's blended map faces in the table's order through
  `DrawMerged`, the merge split out of `DrawWorldWater` (which now calls it too). An instance marked
  `BlendSurfaces` has its blended faces drawn into the normal and surface buffers in the water's
  slices (`DrawBlendNormals`): `WorldNormalVs`'s `uModelBlend` takes a solid one's as opaque, else one
  with a material, or on the water's rect (`FlagWater` on a mesh corner) in an averaging blend
  (`uModelTwin` the twin's rate), as a blended surface; and a blended model surface carries its own id
  plus 256 rather than water's. The probe's surface readback counts samples by id
  (`RetainedScene.SurfaceIds`). With `uModelTile` and `uModelBlend` 0 the programs are the ones
  before. The thirteenth diff in the patch file. See "Step 3, the seventh slice", "Step 3, the
  eighth slice" and "Step 3, the ninth slice" in `docs/GPU_RENDERER.md`.
  Since amended: the map's opaque range is drawn with `0051`'s tolerance, as the models
  already were (`GlMainView.DrawMapOpaque`, main view and mirror): true depth first with
  colour masked, then colour against it pulled towards the camera, so two wall panels
  overlapping in one plane no longer fight. `PrimFs` and `WorldVs` gain `uDepthOnly`, set
  for every such depth pass (`DepthOnly`): the fragment decides only whether it exists,
  and the corner is not lit. With it 0 the programs are the ones before.
  `RetainedScene.MainMapPrepasses` counts the map draws. The fourteenth diff in the patch
  file. See "The map's seams fought again" in `docs/GPU_RENDERER.md`.
  Since amended: the main view latches the display target it draws into (`0024`'s
  `MarginContentFlip`) once the map is drawn with the clip spanning the target. The
  latch counted only packet vertices past the clip, and the map and models drawn here
  are none, so a target built while the GPU drew the world (an aspect changed in
  play) was refused by the present for good: the 4:3 VRAM fallback, and no pass that
  needs a display target. The fifteenth diff in the patch file. See "A target made
  under the GPU world renderer never latched" in `docs/WIDESCREEN.md`.
  Since amended: the depth-stage probe reads this draw's target back, and the
  surface probe's pass runs with the occlusion and reflection features off
  (`RetainedScene.DepthStageProbe`, `RetainedScene.SurfaceCheck`). The sixteenth
  diff in the patch file; the fields are under "Retained contract additions under
  verification" below.

- `0086-retained-model-mask.patch` — the retained models (`0085`) are drawn at slot 1,
  ahead of every packet the walk sends after them, and `0051`'s tolerance (1 unit
  plus half the depth's change across a pixel) hands a coplanar overlap to the later
  packet. Over a model's pixels it therefore let through any packet up to the
  tolerance *behind* the model, such as a near map face's floor behind a creature,
  whose slope term is large at a grazing angle. The display target's depth is now
  `DEPTH24_STENCIL8` (sampled, it still reads as depth). While
  `RetainedScene.ModelMask` is set (**off by default**; the game turns it on), the
  main view clears the stencil before its models and their colour passes set it where
  they land (`MarkModels`), and the target records the frame (`ModelMaskFrame`). In
  that frame a tested packet batch's colour (`GlCore.DrawTested`) draws twice: with
  the tolerance where the stencil is clear, and against the true depth where it is
  set. An opaque batch that draws there clears the mark, so the next packet gets the
  tolerance against it. `RetainedScene.ModelMaskProbe` adds occlusion queries (a
  stall each): over model pixels, a batch's samples the tolerance passes and those of
  them behind the stored depth (`MaskSamples`, `MaskBehind`); a blended batch's
  samples in front by less than the tolerance (`MaskAhead`); and before each model
  list writes depth, its samples against the map and those that only a test pulled
  8/32/128/512/960 units nearer passes (`ModelSamples`, `ModelUnderSlack`). Planar
  captures are untouched. **No recompile.** Measured in Verdite3's `GPU_RENDERER.md`
  ("Models under later packets").

- `0087-retained-depth-ceiling.patch` — `0051`'s tolerance is the constant bias plus
  half the fragment's own depth change across a pixel, and on a face seen nearly
  edge-on that slope term spans hundreds of units: a model's silhouette drew over a
  surface that far in front of it. `PrimFs` gains `uDepthCapZ`: the slope term stops
  at the bias plus `vDepth * uDepthCapZ`. The main view sets it to
  `RetainedScene.DepthCapPixels / H` (`BeginWorldMain`) and clears it with the rest
  of its uniforms, so a capped term is the world width of that many game pixels at
  the fragment's depth. **0, the default, is the program before**; packets
  (`_progPrim`) never set it. `RetainedScene.ToleranceProbe` adds occlusion queries
  (a stall each) before the colour pass of the main view's map, posed models and
  instances (`ProbeTolerance`, through `PrimFs`'s `uDepthCap`, which only it
  lowers): the samples passing (`ToleranceSamples`) and those passing only because
  the tolerance exceeded each of `ToleranceCaps` (`ToleranceBehind`). **No
  recompile.** Measured in Verdite3's `GPU_RENDERER.md` ("Seeing through doors, and
  floor over models again").

- `0088-retained-neighbour-blend.patch` — each map half has its own light record, so
  the static map's colour and fog stepped at every tile edge where records differ.
  `NeighbourBlend` blends them **per pixel** in `PrimFs`: the records of the half a
  pixel lies on and of the three halves around its quarter of the tile (on the same
  level; none past the map or where a half is missing), weighted bilinearly between
  the tile centres from the pixel's world X and Z, so either side of an edge or
  corner blends the same set. The fog averages the records' **results**, each
  record's cue weight at the pixel's own depth clamped at 4096 (the colour it leaves
  is black there): the average of the pictures the tiles would draw, with a record
  that has no fog weighing in as none. Only curve-5 (`LinearDepthCue`) and curve-0
  records blend; another curve leaves the pixel as it was. The light averages the
  records' colour matrix and back colour, rounded to integers, under the own record's
  dots, in `NormalColorCol`'s integers as `recordLit` does; just off a tile's centre
  that rounds back to the own record, so the blend meets the unblended picture without
  a step. A pixel whose records all fog and light alike is drawn as before. `WorldVs`
  passes a record-lit static corner's half, record, RGBC, world X/Z and dots
  (`recordLit` gains an `out` for its dots) while `uNeighbour` is set; `PrimVs` writes
  zeros. The port sets `NeighbourBlend.Mode` (fog 1, light 2) and fills
  `NeighbourBlend.Halves` (each half's record plus one, 160x80, uploaded as R8UI on
  unit 22 when it changes); `BeginWorldMain` sends the mode, and `EndWorldUniforms`
  clears it, so reflections and shadow passes are untouched. `_progPrim` puts the two
  new integer samplers on their own units. **Mode 0, the default, is the program
  before.** **No recompile.** Measured in Verdite3's `GPU_RENDERER.md` ("Blending
  light and fog across tile edges").

- `0089-retained-distance-fade.patch` — the main view could only draw the halves the
  game's walk submitted, at full weight, so a port could neither draw past the game's
  draw radius nor soften the edge where land pops in. Two additions. **The gate**:
  `RetainedScene.CurrentMainHalves` hands the port the current frame's
  `MainHalves`, so it can add halves past the walk's reach to the main view alone (not
  reflected, not the mirror's). **The fade**: `DistanceFade`, per pixel in `PrimFs`,
  weight `clamp((edge - d) / band, 0, 1)` with d the **horizontal** distance from the
  camera's X and Z, so turning on the spot fades nothing and a model fades with the
  ground under it; past a depth limit less 2048 the weight also falls to 0 by the
  limit (`DepthLimit` 65,024), before the world program's 16-bit depth runs out. It
  multiplies 0072's `vFade` into the same ordered dither (`fadeDropped`, the
  crosshatch's table), so there is no blending and a drawn pixel keeps its depth.
  `WorldVs` and `WorldNormalVs` pass the corner's world X/Z (`vFadeXZ`; the camera's
  for the sky's models and a model placed in view space, which never fade); `PrimVs`
  and `NormalVs` write zeros. `NormalFs` drops the same pixels, so AO and the
  surface buffer do not see what the colour pass dithered away. The port sets a
  frame's fade with `RetainedScene.SetFade` (edge, band, depth limit; cleared by
  `BeginFrame`); `BeginWorldMain` sends it to the main view only (`uFade`,
  `uFadeZ`), `EndWorldUniforms` clears it, and `DrawWorldNormals` sends it to the
  normal program. `NormalFs` is now a `static readonly` composed string. **A frame
  with no fade and no added half is the program before.** **No recompile.**
  Measured in Verdite3's `WIDESCREEN.md` ("Render distance").

- `0090-no-system-cnf.patch` — a disc with no `SYSTEM.CNF` could not be recompiled:
  `SystemCfg.Parse` read it unconditionally. With none, the BIOS boots `PSX.EXE`
  with TCB 4, EVENT 16 and the stack at `0x801FFF00`, the class's own defaults, so
  `Parse` now returns them; `DiscProbe.SystemCfgBoot` answers `PSX.EXE` the same
  way, so `--autoconfigure` names the boot file rather than guessing the first
  executable. King's Field (`SLPS-00017`, Verdite1) has no `SYSTEM.CNF`. A disc
  with one reads it as before. **Forces a recompile** only for such a disc.

- `0091-vblank-from-the-poll.patch` — on `0021`'s timeline the vblank is delivered
  only from inside `LibEtc.VSync`, so a game that waits for its own vblank handler
  without calling `VSync` waits forever: King's Field (`SLPS-00017`) spins in its
  frame gate on a counter its `RCntCNT3` handler bumps, and drew four frames after
  the first area loaded. `LibEtc.VBlankFromPoll` (off by default; the port sets it)
  has `Interrupts.PollSlow` deliver the vblanks that are due on the same wall-clock
  grid, inside the poll's register snapshot and exception stack, before draining
  pending IRQs. `AdvanceVBlanks` marks itself running, and a poll that arrives from
  inside a delivery (a handler is recompiled code, and polls too) does not deliver
  again. With the switch off the only change is that mark, so a game that does not
  set it runs as before. Measured in Verdite1: about 20 frames a second in the first
  area, the gate's three vblanks a frame. **No recompile.**

- `0092-wrapped-image-load.patch` — an image load (GP0 `A0`) that runs past VRAM's
  right or bottom edge wraps on the console, and `StoreImageHalfword` wraps it into
  the shadow, but `HleLoadFlush` handed the backend the whole rectangle, and every
  GL backend's `WriteRect` is one `TexSubImage2D`, which refuses a rectangle past
  the texture with `GL_INVALID_VALUE`: the backend received none of it. King's
  Field (`SLPS-00017`) loads its HUD and menu palettes as 16x16 TIM CLUT blocks at
  rows 497-500, so the backend's palettes read zero, every texel transparent, and
  the HUD and every menu were drawn and invisible. A wrapping load now goes to the
  backend as up to four pieces, each at its wrapped position; a load that fits is
  passed as before, so a game that never wraps one is unchanged. Measured in
  Verdite1: 35 palette words differed between the backend's VRAM and the shadow,
  and 0 after, with all of VRAM outside the display buffers equal through the boot,
  the first area and the in-game menu. Fills and VRAM copies that wrap are not
  split. **No recompile.**

## Retained contract additions under verification (2026-10-04)

The depth-linear cue is curve 5 in `LinearDepthCue`, composed into the actual
retained world and fragment programs. Its parameters are a near/far pair, with
quarter-depth quantisation, truncating division, a 32000 cutoff and 7951 maximum.
Quantised integer records use integer division to avoid driver reciprocal
rounding at exact boundaries; fractional pairs remain supported. Existing cue
curves retain their previous behaviour. Fog culling stays conservative for this
curve. No game address or environment variable is introduced in the runtime.

The main retained draw and its normal pass accept a frame with no opaque static
map, so model-only frames can render. The port still owns scene validity.
The numerical surface probe can request the surface attachment without enabling
reflection features; normal/AO-only configurations can therefore measure it too.

The retained world can be sampled for depth at two stages without an image. The
game sets `RetainedScene.DepthStageProbe` (off by default; the port's switch, as
`SurfaceCheck` is), and the runtime then reads the frame target back at most once
every five seconds: immediately after the retained main draw, and again at the
present for the source it is about to show, after the present's surface pass. The
after-main stage reads depth only and counts its surface samples as stale, since
the pass has not run for that frame. The present stage reads normal/surface ids
only where `DrawSurfaces` drew for that present (the probe takes the draw's
success as `surfaceFresh`); a surface attachment left over from an older frame is
counted stale, never read. Both stages record the target, frame, retained serial
and size they read, and the present stage records whether the pair read the same
target and serial. A stage with no target, a target with no depth attachment, and
a sample with no surface buffer are counted as absent rather than reported as
zeros; a pair that did not match still counts its two stages. Every read puts the
read framebuffer, that framebuffer's read buffer and the pack alignment back, and
the normal/surface framebuffer's own read buffer with them, in the surface probe
as well.

The surface probe also runs with the occlusion and reflection features off. With
neither consumer ready, the present draws the surface pass for the probe alone at
the render scale when `SurfaceCheck` or the depth-stage probe's present stage asks
for it, `RenderSurfaces` runs for either switch, and the geometry keeper
(`AoGeometry.Active`) collects the frame's triangles and the retained map's frame
entry for as long as either holds, so that pass has something to read. That is
deliberately not `GteDepth.SurfacesWanted`, which would change what the packet
path writes to the depth buffer, and it leaves `GteDepth.AmbientOcclusion` and
`GteDepth.Reflections` unset.

Applicable late fixes from the Verdite2 vendored copy preserve `NotRect` through
surface classification/depth records and texture repair's dialogue ink holes.
The existing `SurfaceMaterial.Classify` overload remains binary compatible.
The bounded Verdite2 new-game check keeps retained map/poses/sky/water/mirror and
normal passes active with zero reported legacy world projections/3D packets.
Shared checkpoints belong in subtree-only commits and the Verdite fork, per
repository guidance. Shader arithmetic is measured separately from visual acceptance.

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
