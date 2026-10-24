# Runtime

What kf1-enhanced does in place of the PS1's hardware, what that means for
small platforms, and why it draws fewer frames than the recompilations.
Read from the source 2026-10-22, at `ac147450`.

## What is emulated

**Almost nothing.** The game is native code, and each piece of hardware is
replaced by a plain PC equivalent. Sound is the one partial exception.

| PS1 part | in kf1-enhanced |
|---|---|
| CPU | none: the decomp's C/C++, compiled natively |
| GTE | none: the math the game asked of it is ordinary code in the game's 12-bit fixed point (`decomp/src/lib/fixed_math.cpp`), so results match; only conventions remain (`KF_GTE_DEPTH_TO_OT_SHIFT`) |
| GPU, VRAM | none: faces go into a draw list that the renderer frontend batches for Vulkan; textures become materials at load ("this is not runtime emulated VRAM", `decomp/include/kf/lib/codec.h:27`); 1024x512 survives as a texture-coordinate convention |
| CD | none: the files are extracted once into `data/` and read as files |
| BIOS, interrupts, DMA, timers | none: a 60 Hz host clock with waits (`port/platform/host.cpp`) |
| memory card | none: the game's save format in `.kfs` files |
| SPU | **reimplemented at a high level**: `decomp/src/audio/sound.cpp` (709 lines, patched) mixes 24 voices at 44.1 kHz, decodes PS1 ADPCM (16-byte blocks, 28 samples), runs the envelopes and the sequenced music. Not register level, and its reverb is a generic comb and diffuser design, not the SPU's |

The recompilations are the opposite: RecompOne translates the MIPS to C# and
emulates everything around it (the GPU in software into 1 MB of VRAM, the GTE,
SPU, CD with XA, DMA, interrupts, timers, BIOS calls), and much of Verdite's
picture work was getting meaning back out of the emulated GPU stream.

Consequences:

- **Faithfulness rests on the rewrite.** Wherever the game relied on a hardware
  quirk, sushi-shi reproduced it by hand. The reverb is a known approximation:
  a listening comparison against the recompiled Verdite1 belongs on the manual
  pass.
- **Porting is cheaper.** No emulator to port: a new platform needs a renderer
  backend, audio output and a platform layer.

## World rate

The main loop (`decomp/src/game/game.cpp`) steps the world once per
`FRAME_PACER_INTERVAL_TICKS` = 3 ticks of the 60 Hz host clock: 20 updates a
second, the rate of the game's own frame gate (`func_800149F4` on `main`,
`docs/GAME_INTERNALS.md` there). `frame_pacer_wait` restarts from the time it
woke, not from the deadline, so an update that overruns slows the world rather
than catching up. Gameplay > Tick rate (`gameplay.tick_rate`: 12, 15, 20 or
30) sets the vblank count (`kf::world_update_vblanks`, 5 to 2). The default
is 15, by the user's choice (2026-10-23): the gate is a ceiling, the console
fell below 20 wherever a frame took longer than 50 ms to build, and 15 stands
for a typical rate there (not measured). Classic mode keeps the gate's 20.
The game counts updates, not time, so the rate is the game's speed: 15 is 25%
slower than 20 in walking, turning, enemies and timers. Music is timed on the
host clock and keeps its tempo. Verdite1 on `main` also defaults to
`kf1.tickrate=15`.

## Small platforms: the 3DS

Sound is not a concern. Estimated, not measured: 24 voices at 44.1 kHz is about
1.06 M samples a second; at a generous 30-50 cycles a sample for decode, pitch,
envelope and mix that is 30-50 M cycles a second, 10-20% of the original 3DS's
268 MHz core, and only with every voice playing. Reverb adds a few percent. The
PS1 did it with a 33 MHz CPU and the SPU; native code has room to spare.

The 3DS's DSP can also do most of it. It has 24 channels, as many as the SPU's
voices, with hardware pitch, volume and mixing. It does not read PS1 ADPCM, so
samples are decoded to PCM16 when a bank loads. SPU RAM is 512 KB, so all of it
decoded is at most about 1.8 MB. The CPU is left with envelopes once a tick,
plus reverb, which can run on the second core.

The real 3DS limits:

- **Memory**: about 64 MB for an app on the original 3DS; the disc's files are
  about 26 MB. Decode an area on entry, not everything at boot.
- **GPU**: Classic and Balanced tiers only ([RENDERER_PLAN.md](RENDERER_PLAN.md)).
- **Float**: the ARM11 has a basic VFP and no SIMD; the renderer frontend's
  per-vertex float work needs profiling there. The game logic is integer fixed
  point and is fine.
- **Screens**: 400x240 fits 320x240 nearly 1:1, with 5:3 widescreen; the real
  depth makes stereoscopic 3D nearly free; the bottom screen suits the automap.

The original 3DS is a plausible target, to be measured once a build runs there.

## Frame rate: why the recompilations draw more

The user's report (2026-10-22, uncapped, vsync off, `frame_rate: -1`, an RX
9070): **kf1-enhanced draws about 300-400 frames a second; Verdite3 draws
800-1200.** The gap is the renderer's GL submission, not the decomp. Read from
the code, not profiled, so the shares are estimates. That renderer was deleted
on 2026-10-23 for the Vulkan one, built by the rules below; the paths are as
they were before then.

1. **One draw call per triangle.** The main loop in
   `port/renderer/renderer.cpp:1004-1024`, for every triangle: toggles depth test
   and depth writes, binds its texture, sets 3-4 uniforms (texture mode, dither,
   blend mode, UV bounds), uploads its 3 vertices with `glBufferData`, and calls
   `glDrawArrays(GL_TRIANGLES, 0, 3)`. A room is a few thousand triangles, so
   tens of thousands of GL calls a frame on one thread: about 3 ms at a
   microsecond each, which is where 300-400 fps sits. The GPU is mostly idle.
2. **A screen copy per blended triangle** that reads the destination
   (`renderer.cpp:993-996`, `glCopyTexSubImage2D` of its bounds), each a
   pipeline stall.
3. **Smoothing on the CPU.** Every displayed frame, `smooth_present`
   (`port/platform/host.cpp`) copies the last tick's face list and re-projects
   every vertex single-threaded, then the per-triangle loop runs again in full.
4. Smaller: the shadow cube maps redrawn for every shadowing light every frame;
   `glGetUniformLocation` looked up every frame in the post passes.

Verdite3 through RecompOne draws batched geometry and keeps static map geometry
in cached GPU buffers, hence the difference.

The fixes are design rules for the new renderer, not patches to this one:

- **Batch.** One vertex buffer per frame, the sort kept, one draw per run of
  triangles sharing state. Per-triangle settings become vertex attributes;
  textures go in an atlas or array so binds disappear.
- **Fixed-function blending** for all four PS1 modes: no screen copies.
- **Smoothing in the vertex shader.** Each vertex already carries its
  camera-space position (`cx/cy/cz`): two matrices and a blend factor, nothing
  on the CPU between ticks.
- **Static geometry resident**: each area's map uploaded once; only models and
  effects per frame.
- **Shadow maps redrawn only** when a light or the geometry near it changes.

Two ways to confirm before the rewrite: sample the running game with `perf`
standing in a room, or batch the current GL renderer and see how much of the
gap the per-triangle draws explain.
