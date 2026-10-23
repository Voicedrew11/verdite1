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

### The exact view rotation

The game's view matrix is `matrix_set_rotation_xyz` in 12-bit fixed point: the
sine table is exact to half a unit, but the truncating shifts leave entries up
to about 6/4096 off a rotation, and off differently at each angle. Corners then
move against each other as the view turns, by up to 0.1 px at 320x240 (about
1 px at 4K), which reads as a residual texture warble even with perspective
and sub-pixel on. Smoothing used to undo the matrix with its transpose, which
is not its inverse, and redo it with the same flawed matrix. Vulkan now undoes
it with its true inverse and sees the world through the exact rotation for the
same angles (`view_rotation.h`), for the map, models and lights alike; with
smoothing off, sub-pixel pictures are reprojected the same way
(`ViewSmoothing::view_exact`), and whole-pixel (Classic) ones keep the game's
matrix. The GL renderer still draws with the game's matrix.

### Detailed distant walls

Each map attribute has two meshes in MIXB.DAT's map TMD: object n for the
nearest 3x3 cells of the cell window and n + 100 for the rest. The distant
one has a quarter to a seventh of the faces (B1: 28 against 4 for a plain
wall), and a texture page cannot tile, so each texture is stretched over the
larger faces: a wall changed pattern as the player walked up to it. Display >
Detailed distant walls (on; off in Classic mode) draws every cell with its near
mesh (`render_detailed_map`, patched into both `render_map_cells.cpp`).

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

### Mipmaps

A layer is a whole 256 x 256 page with its palette, holding many faces'
rectangles side by side, so a plain mip chain would bleed neighbours into a
face. Verdite gave each rectangle (later each aligned square holding it,
`MipShare`) its own atlas entry. Here each layer keeps one chain
(`texture_mips.h`): colour weighted by coverage (transparent texels are
black), so a cut-out's edge keeps its colour, and STP moved to alpha 254 so
coverage stays near 1. `world.frag` caps a face's level at the smallest aligned
square holding its rectangle, which is what `MipShare`'s entries held, and
keeps a filtered sample's taps inside the rectangle. Transparency and STP come
from the nearest level-0 texel. Bilinear is the hardware's trilinear, and
anisotropic (Display > Anisotropic filtering, Enhanced, 16x by default as
Verdite judged, held to what the device has) when that is on: the level comes
from the short axis of the footprint, or the long one over the anisotropy, as
the hardware picks it, so the cap holds. Nearest keeps the original texel while
it covers a pixel or more and fades into that trilinear sample as it shrinks to
half a pixel: picking one level by hand drew a hard line across slanted floors
and ceilings where level 0 gave way to level 1, and took that level from the
footprint's long axis, so the line came close. Three-point filtering picks the
nearest level by hand. `kf_vulkan_tests` checks a minified checker averages
with nothing from the square beside it, a cut-out keeps its colour and edge,
16x keeps stripes across a face squeezed 16 to 1 that 1x blurs, with nearest
and bilinear, and nearest draws a face whose texels cover a pixel or more as it
does without mipmaps.

Sharp filtering (Texture filtering > Sharp, the default; GL and Vulkan) is
nearest without its uneven texels. A texel magnified to a fraction of a pixel
per texel, say 2.4, comes out 2 or 3 pixels wide under nearest, and the wide
ones read as chunky clumps. Sharp takes the bilinear taps but keeps each weight flat
inside the texel, blending only across the one screen pixel its edge crosses,
so every texel has the same weight. Past one texel a pixel it is bilinear, and
with mipmaps it fades into trilinear as nearest does. Transparency still comes
from the nearest texel, so cut-out edges stay hard. `kf_vulkan_tests` checks
stripes drawn 9, 9 and 10 pixels wide by nearest each weigh 9.375 pixels under
sharp, and sharp is nearest away from a texel's edge. It was made for textures
looking chunkier once the camera stops than while it moves, and did not cure
that: once the camera stops the frames are identical, so what changes is not
in the picture.

### Enhanced distance

Verdite's `PlainDepth` (`KF1_ENHANCEDIST`): past Display > Enhanced distance
(cells, Enhanced; 0, everywhere, by default as on `main`), `world.frag` draws
world faces the game's way by their camera depth: the nearest level-0 texel,
no ambient occlusion, the game's fog at the corners. A hard switch, as
Verdite's was. Dynamic lights are not part of it yet. `kf_vulkan_tests` checks
fog and filtering either side of the depth.

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
   2026-10-23: `port/renderer/vulkan/` draws Classic and the Balanced options, offscreen-tested in `kf_vulkan_tests` on
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
