# Renderer plan

The plan for the decomp ports' rendering stack, as discussed 2026-10-22. It
replaces kf1-enhanced's GLES 3 renderer, which stays as a reference to check
the new one against. Why the current renderer draws fewer frames than the
recompilations is in [RUNTIME.md](RUNTIME.md).

## Platforms

Windows, Linux, macOS, Android, Nintendo Switch and Nintendo 3DS.

| platform | what it runs | Vulkan |
|---|---|---|
| Windows, Linux | Vulkan, GL, D3D12 | native |
| Android | Vulkan (required on 64-bit devices since Android 10), GLES | native; old or cheap devices have weak drivers |
| macOS | Metal only (GL frozen at 4.1) | through MoltenVK onto Metal; a few features missing |
| Switch (homebrew) | deko3d (low level, close to NVN), or GL through devkitPro's Mesa | **no**: only licensed developers get it, through Nintendo's SDK |
| 3DS | citro3d on the PICA200 | **no**: vertex shaders, but fragments go through fixed combiner stages; not even GLES 2 class |

Vulkan reaches four of the six, so the decision is the structure: one frontend
that decides what is drawn, and one backend per API that draws it.

## Structure

```
decomp game code --> renderer frontend --> backend
                     (portable C++)        |- Vulkan   Windows, Linux, Android, macOS
                     - scene, materials    |- deko3d   Switch (or GL via Mesa)
                     - per-tier features   '- citro3d  3DS
                     - draw lists
```

The frontend speaks the game's terms: textured, gouraud and flat faces, the
four semi-transparency modes, palettes, map cells, models. It hands a backend
finished draw lists. Every Verdite feature lives in the frontend or in shaders
written once, never in backend-specific logic. kf1-enhanced's draw list (whole
faces with camera-space positions, material and blend mode, sorted before
drawing; `port/include/kf/renderer/renderer.h`) is the starting point.

The backend interface stays narrow (draw lists, materials, render targets),
so another API later touches one directory.

## Tiers

The frontend picks a tier per device:

| tier | contents | where |
|---|---|---|
| **Classic** | the PS1 picture exactly: the game's own per-vertex lighting, the blend modes, palettes decoded to textures | all, 3DS included |
| **Balanced** | plus depth buffer, perspective, sub-pixel, true colour, filtering, mipmaps, widescreen, smoothing | all; on 3DS, New 3DS for speed |
| **Enhanced** | plus per-pixel lighting and fog, AO, anisotropic filtering, render distance | Vulkan and deko3d |

Classic is the game's own output: the decomp already computes the original
per-vertex lighting on the CPU, which is what makes the 3DS cheap. Enhanced
replaces it with shaders where they exist.

## Why Vulkan for the main backend

What it gives:

- **Validation layers**: misuse, synchronization hazards and wrong resource
  usage are reported instead of tolerated. Valuable for agent-written code.
- **Shaders compiled ahead of time** to SPIR-V, instead of GLSL compiled by
  each driver, which is where most "works on one vendor" GL bugs come from.
- **Explicit state**: no hidden global state; a frame's behaviour is in the code.
- **Present modes chosen by us** (FIFO, mailbox, immediate), which the smoothing
  work needs.

What it does not give: identical pixels across vendors (rasterization,
precision and filtering vary within the spec's tolerances), freedom from
Android driver bugs (fewer, not none), or native macOS (MoltenVK).

The cost is setup code, kept down with **volk** (function loading), **VMA**
(memory), **vk-bootstrap** (instance and device), and **dynamic rendering**
(no render-pass objects; core in 1.3, an extension on 1.1). Baseline: **Vulkan
1.1, plus dynamic rendering where available**, which reaches most Android
devices and works under MoltenVK. Validation layers on in development.

SDL_GPU (SDL3's own explicit API over Vulkan, D3D12 and Metal) was the
alternative: less code and native Metal, but less control, and it does not
reach the Switch or 3DS homebrew either.

## Shaders

Written once in GLSL or Slang. SPIR-V for Vulkan; deko3d compiles GLSL with its
own `uam`; SPIRV-Cross or Slang can emit MSL if macOS ever gets a native Metal
backend. The 3DS gets no shaders from this pipeline, only a few hand-written
combiner setups for the PS1 blend modes, which is all Classic needs.

All four PS1 blend modes are fixed-function blending, so no backend needs to
read the screen back: ½B+½F (constant factor 0.5), B+F (additive), B−F
(reverse subtract), B+¼F (constant factor 0.25).

Decided 2026-10-23: Classic blends with the hardware too, into a 15-bit target
with the source cut to 5 bits first, though the GL renderer copies the screen
per translucent triangle to match the PS1's truncation exactly. The rest is a
rounding difference of at most one 15-bit step, and a readback path would be
Vulkan-only (the 3DS cannot read back) and slow. If a case shows in play (a
fade or a retained frame blended repeatedly that settles off), it gets its own
fix.

## Carrying over Verdite's picture

Most of Verdite's picture work existed to recover what the GPU packets threw
away: depths for the Z-buffer, sub-pixel fractions, map cells told apart from
models, packets matched across ticks for smoothing, the address map. In a
decomp the renderer is the producer, so that information is at hand, and the
work is rendering, not reverse engineering.

| Verdite feature | in the decomp |
|---|---|
| true colour, no dither, sub-pixel, perspective, Z-buffer | free: float vertices with real depth; kf1-enhanced has them |
| retained map, map cache | build each area's static cells into GPU buffers once, keyed by area. Kept 2026-10-23, knowing the cost: the game projects and lights map cells on the CPU each update, so this moves that work into the renderer. Not in the first backend, which uploads each update's faces once |
| smoothing (view, models, HUD, item display) | done on Vulkan (2026-10-23), below. Positions blend between the last two updates, but the view turns to the newest look input each displayed frame, so looking has no update (50 ms) of lag |
| per-pixel lighting, distance fog | port the game's own lighting and depth-cue routines, named in the decomp, into the shader with the game's parameters: the original look, evaluated per pixel |
| AO with geometry normals | done on Vulkan (2026-10-23), below |
| mipmaps, anisotropic, mip sharing | decode each texture page and palette to RGBA textures; watch textures the game rewrites in video memory (animated textures, palette changes), which need invalidating |
| render distance with fade, widened cull | the cell loop and stencils as parameters; keep **render reach separate from logic reach**, so enemy activation and events do not move |
| widescreen HUD, menus, tints | laid out at the source, not stretched afterwards |

### Smoothing: what is carried between updates

Verdite1 carried each system by hand (`docs/SMOOTHING.md` on `main`): the
camera, five model tables' positions and angles, the animation clips' times,
the arm, the HUD bars, the item display's turn, the warp column's scale. It had
to, because it saw only the GPU packets. Here the game's own projection is at
hand, so Vulkan carries vertices instead (`vertex_carry.h`), the same way for
everything:

- **Models in the world** (map objects, actors, effects, people): each call of
  `tmd_project_vertices`, and each world sprite, hands over its model-to-camera
  matrix and its posed vertices. The carry keeps the model-to-world transform
  of both updates and each vertex's way back in model space; a displayed frame
  turns the model along the shorter arc, blends its scale and position, and
  blends each vertex along its way back (`model_transform.h`). The turn and
  the scale are the game's own: a door's swing, a pickup tipping over, the warp
  column spinning 0x200 a step and growing from height 0, which a blend of
  positions would shrink. A pose is a linear blend of two keyframes in the
  game, so blending the posed vertices is what the game would draw in between.
- **Camera-fixed and 2D** (the held weapon's swing, the item display, the HUD
  bars): each point's way back on screen.
- **The compass needle** is not carried: it turns about its pivot by the yaw
  the shown view has turned since the update, so it points where the shown
  view faces, look input included.

A drawing is matched to the one before by the thing's tag (`KF_RENDER_TAG_*`),
the nth drawing under it, an identity (object id, actor definition, effect
kind and type, person's model, the item display's queue slot) and the vertex
count. A step over 2000 units or a turn over 0x300 in one update, as Verdite
judged, carries nothing; a warp of the camera carries nothing at all.

Not needed in the decomp, since its game logic runs once per update: the
flipbook sprites stepping on every drawn frame, the item display's hold, tints
and textures. The GL renderer keeps kf1-enhanced's rigid per-entity smoothing
(`host_report_entity`).

### Ambient occlusion and the surface buffer

Vulkan draws the opaque world faces a second time each frame, with a depth
test, into a surface buffer (`vulkan_surfaces.h`): camera depth and each face's
own normal, from the derivatives of its camera position. Verdite had to take
the visible surface from painter's order and, until 0058, normals from depth
differences; here the faces are at hand, so the buffer holds the nearest
surface even where the game's ordering is wrong. Anything else that needs the
world's shape on screen (per-pixel lights, fog by distance, glow's covering
test, FXAA's world mask) reads this buffer rather than measuring again.

AO (`vulkan_ambient_occlusion.h`) is Verdite's: the spiral with a 4x4
interleaved rotation, the 4x4 depth-aware box that cancels it, radius 512,
strength 0.8 at 100%, bias 0.08, nothing past depth 24000, and its tiers
(Low: game pixels and 8 samples, Medium: 2x2 per game pixel, High: the target's
scale, both 16). Unlike Verdite it is not multiplied over the finished picture:
`world.frag` darkens a fragment's shading before the 15-bit step, and only
where the fragment's depth matches the measured surface, so the HUD, the held
weapon and translucent faces are left alone without a mask. Verdite's
floor-plan term (`AoWorld`) is not carried over: judged not worth having.

### Fog per pixel

The game fogs on the CPU: each corner's depth cue (`render_project_point`,
the GTE's DQA/DQB) moves its lit colour toward the fog colour, and the GPU
blends the result across the face. The writers do not all cue alike: map cells
and models use twice the corner's cue (`p2 = fog << 1`), sprites the cue
itself, floor items half as much again. Display > Fog (Enhanced; Vulkan
draws it at Balanced) keeps that: each writer hands the colour before its cue
too (`DepthCuedColor`, `lighting.h`) and the face's scale
(`DrawFace::depth_cue_scale`), and `world.frag` evaluates the same curve at
the pixel's camera depth (`Depth`) or its distance to the eye (`Distance`,
Verdite's judged default), with the update's fog colour and start. `Corners`
draws the game's own colours, so Classic is unchanged. The opening's writers
hand no clear colour and stay at their corners. `kf_vulkan_tests` checks the
curve against the game's arithmetic at a corner's depth, off the view's
centre, at a sprite's scale and for an uncued face.

What carries over is decisions and references, not code:

- **The judged values** (decided 2026-10-23: the starting defaults, judged
  again once Enhanced draws, since other shaders made them): the `MANUAL_PASS` verdicts on `main` and the defaults
  set from them (fog mode, AO strength and quality, fade band, render
  distance) become the defaults here, on by default.
- **A faithful Classic**: with enhancements off the picture matches the PS1.
  Decided 2026-10-23: judged by the user's eye and by playtesting, not by
  comparing captures with the recompiled builds. The agent's checks are the
  offscreen pixel tests of the backend's own arithmetic (`kf_vulkan_tests`).

## Order

1. Define the frontend interface and the tiers: `port/include/kf/renderer/frontend.h`
   (what the game and host call) and `backend.h` (what an API implements).
   A tier is what a backend can draw; options above it are off there.
   Agreed 2026-10-23. Lights are handed over unflickered and flicker on every
   displayed frame; VSync is a picture option the backend turns into a present
   mode; `renderer_capture` keeps `KF_CAPTURE` working on every backend.
   Shaders are GLSL, compiled to SPIR-V with glslang at build time.
2. The Vulkan backend on Linux, validation layers on: reach Classic (judged
   in play), then Balanced, then Enhanced at the judged values. Batched from the
   start (see [RUNTIME.md](RUNTIME.md)). The GL renderer keeps drawing until
   Vulkan reaches Classic, chosen by the Renderer setting (Display, applies on
   restart; `KF_RENDERER=gl|vulkan` overrides it); then GL is deleted.
   2026-10-23: `port/renderer/vulkan/` draws Classic and the Balanced options
   but mipmaps (no Display row yet), offscreen-tested in `kf_vulkan_tests` on
   RADV and llvmpipe with validation on, and the game draws with it, overlay
   included, when Renderer is Vulkan; Classic waits to be judged in play.
   Supersampling draws the target N times larger and averages each N x N block
   before the window (`vulkan_downsample.h`); captures keep the full target.
   It needs dynamic rendering (`VK_KHR_dynamic_rendering`); render-pass
   objects for devices without it are not written.
3. A second backend early, before the frontend sets in Vulkan-shaped habits.
   deko3d is the more useful second one; the 3DS is so limited it would bend
   the frontend.
4. The 3DS last, as Classic and Balanced.
