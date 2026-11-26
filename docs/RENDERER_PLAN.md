# Renderer plan

The plan for the decomp ports' rendering stack, as discussed 2026-10-22. It
replaced kf1-enhanced's GLES 3 renderer, deleted on 2026-10-23 (it is in the
history before that day, and in kf1-enhanced, to check against). Why that
renderer drew fewer frames than the recompilations is in [RUNTIME.md](RUNTIME.md).

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
(memory), **vk-bootstrap** (instance and device), and one-subpass render
passes behind a small module (`vulkan_render_pass.h`). Baseline: **Vulkan 1.1
core, no extensions**, which reaches the Android devices in
[ANDROID_PLAN.md](ANDROID_PLAN.md) and works under MoltenVK. Validation layers
on in development. The backend drew with dynamic rendering until 2026-10-25; it
was replaced on every platform, not kept beside render passes, so that the path
the Mali-G52 handhelds need is the one the desktop tests run.

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
with the source cut to 5 bits first, though the GL renderer copied the screen
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
| per-pixel lighting, distance fog | port the game's own lighting and depth-cue routines, named in the decomp, into the shader with the game's parameters: the original look, evaluated per pixel. Both done on Vulkan (2026-10-24), below |
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
and textures.

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
matrix.

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

### Vertex ambient occlusion

Display > Lighting > Vertex ambient occlusion (off by default and in Classic,
60% in the Balanced and Modern setups; added 2026-11-25) bakes occlusion into the map cells' corners
(`kf/platform/map_occlusion.h`). It is measured from the meshes, not the grid:
the grids hold floor heights, attributes and turns, and the walls are in the
meshes, so a grid term has to guess what is solid, which is where `AoWorld`
went wrong. For each FT3 and FT4 face corner of a drawn cell, 32
cosine-weighted rays (`corner_occlusion.h`) go out over the face's own normal
against every FT3 and FT4 face of the 3x3 near meshes around it, each hit
counting by how near it is, 1 at the corner and 0 at 1000 units (half a cell).
The falloff keeps a small room from going dark all over, `AoWorld`'s other
fault. Rays start 8 units off the face, more than the 4 by which neighbouring
cells' faces overlap (B1-B3 meshes span -4..2004), and 16 in from the corner.

Every cell is baked when a floor loads (`map_load_floor`, after
`map_restore_floor_state` has put back the region copies a save holds), in the
cell's own frame and keyed on its layout: its mesh and turn, and the 3x3 cells'
attributes, turns and floor heights relative to its own. A cell the game
changes later (`map_object_pool.cpp`'s region copies), a mod's change or the
Illusion Staff's swapped mesh is a new layout, baked when it is first drawn.
Turned on during a floor, the whole floor is baked at the next drawn cell.
Distant meshes (Detailed map off) are drawn without it: they are a separate
mesh per cell, far off and fogged, and baking them would double the load's
work or bake as the player walks. The floor's meshes are decoded once on the game thread; the
bake then reads only those, the grids and the quarter-turn matrices, so it runs
on every hardware thread while the game thread waits. The near
meshes have 7 rows of vertices up a 3000-unit wall and split floors at half a
cell, so a crease fades over the face rather than darkening it whole.
`render_enqueue_map` puts each corner's value on `Vertex::occlusion`, carried
in `GpuVertex`'s spare half of the light-set attribute, and `world.frag`
multiplies it into the same light the screen-space term darkens, so it holds
under per-pixel lighting and fog, and stops at the enhanced distance.

Measured 2026-11-25 on B1 (i5-11400, 12 threads): 6034 cells but 3887
layouts, since the floor heights around a cell vary too much for many to
repeat; 1361 ms on one thread, 180-190 ms on twelve, the results bit for bit
the same (a checksum over every layout). Baking a corner shared by faces once
saves about 2%; it is kept so a cell's shared corners agree. A windowed run
with smoothing on baked nothing after the floor's load. Baked as cells were
drawn instead (the first version), the floor's first frame took about 32 ms
for its 150 cells. About 290 occluder triangles a cell; half the corners fully
open and none above 90%, so no mesh's normals face into the rock. Bakes at
import, cached on disk, were considered and not taken: the load-time bake is
under a fifth of a second, and a cache would need a format, a version and
invalidation by the floor's files. `kf_port_tests` checks an open floor, a
crease against a wall fading with distance, and a neighbour's face 4 units
proud.

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

### Lighting per pixel

The game lights each corner on the CPU (`render_light_normal`, the GTE's
NCDS): its light matrix turns the corner's normal into three dots, each held to
0..32767, the colour matrix and ambient turn those into an intensity, and the
packet's colour is scaled by it. The GPU blends the results across the face.
Verdite1 on `main` could only take the three dots the GTE was handed and blend
those, which is the normal blended without renormalizing. Here the writers have
the normals: Display > Per-pixel lighting (Enhanced, on) has
`render_enqueue_tmd`, its retextured pass and `render_enqueue_map` register
how they light (`host_light_set`: the light matrix, colour matrix, ambient and
the model-to-camera transform) and give each corner its model-space normal and
the colour lit (`render_face_light`). `world.frag` blends the normal, holds it
to the length blended from its corners (so at a corner it is the game's
normal), and does the rest of `render_light_normal` at the pixel, then the fog:
per pixel as above, or at the corners by as far as their cue moved their
colours. Faces with no set (sprites, the opening, floor items) are drawn as
before, as is everything past the enhanced distance.

What it changes is Gouraud faces, which are models': map cells are all flat
(FT3, FT4), lit from one normal, and look the same. The game's lights are
fixed in the world, three directions and an ambient; a carried model's light
matrix is turned back by its turn since the update (`light_sets.h`), so the
light stays put as the model turns between updates rather than stepping with
them. Each set also carries the turn of its normals to the shown camera, for
point lights to use. 4096 sets a frame; past that, faces are lit at the
corners. `kf_vulkan_tests` checks a face's corners against
`render_light_normal`, that the middle of a face turning from a light gets only
the ambient where blended corners get half the light, fog at the corners, and a
face without a set; `kf_port_tests` checks the turned lights.

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
footprint's long axis, so the line came close. The fade is measured along the
footprint's long axis, not the anisotropic footprint the hardware's level comes
from: nearest takes one tap, so on a ceiling seen at a low angle, a texel a
pixel across and six down, it skipped five texels in six while 16x anisotropy
held the footprint under one texel, and the ceiling stayed frozen speckle that
only motion averaged out. Three-point filtering picks the
nearest level by hand. `kf_vulkan_tests` checks a minified checker averages
with nothing from the square beside it, a cut-out keeps its colour and edge,
16x keeps stripes across a face squeezed 16 to 1 that 1x blurs, with nearest
and bilinear, a checker squeezed 16 to 1 averages to grey under nearest and
sharp, and nearest draws a face whose texels cover a pixel or more as it does
without mipmaps.

Sharp filtering (Texture filtering > Sharp, the default) is
nearest without its uneven texels. A texel magnified to a fraction of a pixel
per texel, say 2.4, comes out 2 or 3 pixels wide under nearest, and the wide
ones read as chunky clumps. Sharp takes the bilinear taps but keeps each weight flat
inside the texel, blending only across the one screen pixel its edge crosses,
so every texel has the same weight. Past one texel a pixel it is bilinear, and
with mipmaps it fades into trilinear as nearest does. Transparency still comes
from the nearest texel, so cut-out edges stay hard. `kf_vulkan_tests` checks
stripes drawn 9, 9 and 10 pixels wide by nearest each weigh 9.375 pixels under
sharp, and sharp is nearest away from a texel's edge.

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
   start (see [RUNTIME.md](RUNTIME.md)). The GL renderer was to keep drawing
   until Vulkan reached Classic; 2026-10-23 the user judged Vulkan better than
   GL in play, gaps still to fix, and GL was deleted with the Renderer setting
   and `KF_RENDERER`. Headless runs draw through the null backend.
   2026-10-23: `port/renderer/vulkan/` draws Classic and the Balanced options, offscreen-tested in `kf_vulkan_tests` on
   RADV and llvmpipe with validation on, and the game draws with it, overlay
   included.
   Supersampling draws the target N times larger and averages each N x N block
   before the window (`vulkan_downsample.h`); captures keep the full target.
   Every pass is a Vulkan 1.0 render pass (2026-10-25).
   Screen shaders (2026-11-17, issue #15): with Display > Screen shader set,
   the picture reaches the window through a fragment shader into its letterbox
   (`vulkan_screen_pass.h`) instead of the blit. Built in: Scanlines, CRT and
   CRT curved (Timothy Lottes' public-domain shader), CRT (Hyllian), Sharp
   bilinear (public domain) and an LCD grid. The libretro CRT shaders most
   people know (crt-guest-advanced, crt-royale, Mega Bezel, crt-geom) are GPL
   and stay out of this MIT repo; Sony Megatron carries no licence at all.
   crt-hyllian is MIT: its two passes (pass0, pass1) are ported with their
   notice, the mask, curvature, scanline strength and brightness in settings;
   its colour LUTs, glow and NTSC passes are not (no licence in those files).
   A two-pass shader filters along the lines into an image the letterbox's
   width first (`ScreenPass`'s lines). The
   CRT and LCD shaders work on the game's own grid (columns x 240), whatever
   the resolution. Not locked by Classic mode. Not yet: loading a player's own
   shaders (would need glslang at run time) and a row on the in-game settings
   pages at first; they now have a SCREEN page (shader, mask, curvature). A new shader is a new
   `shaders/screen_*.frag`, an entry in `ScreenShader` and in the pass's table,
   and a choice in `display_screen` (`options.cpp`).
3. A second backend early, before the frontend sets in Vulkan-shaped habits.
   deko3d is the more useful second one; the 3DS is so limited it would bend
   the frontend.
4. Android before the 3DS (decided 2026-10-25): the Vulkan backend on mobile
   drivers, in [ANDROID_PLAN.md](ANDROID_PLAN.md).
5. The 3DS last, as Classic and Balanced.
