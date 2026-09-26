# The remaster: authoring tools first, effects second

**A design document, and from Phase 1 on a record of the work done against it.**
Phase 1 is in, in two slices (see "Phase 1, the first slice" and "Phase 1, the
second slice" under the roadmap), with materials since keyed by face ("Faces, picked
from the frame"); Phase 2 has its first slice ("Phase 2, the first slice"), and so
does Phase 3 ("Phase 3, the first slice"); everything else is still design. The
stretch goal is a full visual remaster the user authors themselves: placing lights,
assigning materials, tuning reflections, editing levels. **An effect is worth
nothing to that goal until it can be placed, tuned, saved and shared**, so this
document plans the authoring tools and the data they write first, and treats each
rendering feature as something those tools drive.

Every claim about the existing port is marked the way the rest of `docs/` marks
them: **Confirmed** (measured, or read from the code), **Inferred** (fits the
evidence, not tested) and **Open** (not known). Every planned feature keeps
"mechanism measured" apart from "picture judged". Features ship **off**, and off
must be bit-identical to the picture the port draws today.

The request this answers arrived with some lines cut off. It was read as asking
for:
- each phase's measurement that proves it works;
- the cost of each feature, on and off;
- the remaster being off by default;
- the seams between the layers;
- versioning, and how a pack layers over the base game and over other packs;
- in-game or external editing;
- driving the game with `KF2_SHELL`, `goto`, a free camera and pause;
- forward or deferred lighting.

If one of those readings is wrong, the section built on it is the one to revisit.

## What this is, and what it is not

**"Full visual remaster" needs a target before it needs a renderer.** Two
remasters could come out of this port. One keeps the game's look and adds light,
material and surface detail to it. The other replaces the art. The tools are the
same for both, but the order of the phases is not, and neither is what "done"
means. **Recommendation:** take one area from start to finish before spreading
across all of them. `fdat02` is the obvious one:
- it is where `KF2_AUTOSTART=new` stands, so the spawn is fixed;
- it has water, so the reflection passes were measured there;
- the SSR and planar readbacks already have numbers for it.

Area 1 with save slot 2 (`KF2_AUTOSTART=2`) is the second test scene, as it is
for everything else.

**What this document deliberately does not do:**
- It does not design a new renderer. Everything here extends the passes the port
  already has.
- It does not replace creature meshes.
- It does not add walkable geometry beyond what the tile grid can already express.

Each of those gets a paragraph below saying why.

## What exists, and what each piece gives the remaster

**Most of the machinery a remaster needs is already in the tree, because the
port learned to name its geometry before it had a reason to.** The C# assemblers
know which routine built each packet. The two walks know which tile and which
model. The surface buffer already stores a material per pixel. What is missing
is anything authored, and anything that remembers it.

| piece | what it is | what it gives the remaster |
|---|---|---|
| `Gpu/SurfaceMaterial.cs` (`0067`) | a per-pixel material id stored in the surface buffer's alpha, 8 slots, with a `Reflectivity`/`F0` table the port fills | **The material channel.** The id already reaches the reflection pass, and "What a surface is made of" in `RENDERING.md` names lighting as its next reader. Eight ids is too few to author with, so widening it is an amendment to `0067` |
| `GtePacketDepth` (`0050`) | per-packet corner depths, `Solid` and **`Material`**, written by the C# assemblers | **Where authored per-tile and per-model materials are written.** `SurfaceMaterial.Classify` already prefers the packet's material. **Confirmed**: `PolyAssemblerDepth.SealDepth` is the one place a record is sealed, and it never sets `Material` |
| `GteLightMap` (`0048`) | per-corner lit colour or light dots and the raw depth cue; `BK`/`LCM` by generation | **The chain authored lights join.** `shade8` rebuilds the game's colour per pixel from these inputs; an authored light adds a term to it |
| `GteTexRect` (`0060`) | the texture rectangle of a face, for clipped fans | **A stable rectangle for a texture key**, where a clipped fan's own UVs would give a different rectangle every frame |
| `patches/TileWalk.cs` | the 24×24 cell sweep, a cell's two halves, one half set up and assembled | **Tiles, by coordinate, as they are drawn.** It knows `(x, z, half)`, the world position and the assembler. It does not *publish* the current tile yet, the way `ModelWalk.SetSubmit` publishes the current model |
| `ModelWalk.Scene` | a `ModelDraw` per submit: kind, slot, record, model id, world position, assembler, light record | **Models and instances, by table and slot, with a world position.** Used for picking, per-model materials and light receivers. `SubmitKind`/`SubmitRecord`/`SubmitSlot` are live while a packet is being built |
| `patches/PlanarWalk.cs` (`0068`) | the tile walk run again, and the model submits replayed, from a camera mirrored in the water, with every piece of state saved and restored | **Proof that the scene can be drawn from a second camera without disturbing the first.** It is the template for the editor's free camera, and for any later pass drawn from a light's point of view |
| `patches/AoWorld.cs` (`0059`) | the area's 80×80 tile grid as a GPU texture, and the camera's rotation and world position as uniforms | **World space inside a shader.** It turns a fragment's view position into a world one, and it is a grid a shader can march, which gives coarse shadows for authored lights |
| `patches/Map*.cs` | the full-screen plan, the docked panel with a readout of all ten tile bytes under the cursor, fog of war, markers | **The editor's top-down view and tile picker, mostly written.** It also holds the one invariant that proves the tile addressing: the player stands on a drawn half with a floor-height gap of 0 |
| upstream `Assets/` | texture and XA replacement packs: `pack.json`, content-hashed texture keys, `TextureDumper`, a Texture Inspector panel | **Texture identity and replacement images, already built.** See below |
| `FrameCapture` (`0046`) | one run of stage 13, scrubbed GP0 command by command, with an owner routine for each command | **Picking a texture on demand**: which command drew this pixel, and so which texture page, CLUT and UVs |
| `OutputView` (`0029`) | where the game picture sits in the window, in window pixels and in game pixels | **Where the editor draws its gizmos** |
| `KF2_SHELL` and `mcp/` | `goto`, `state`, `nearby`, `load`, `warp`, `press` over TCP | **How an agent drives every measurement below without seeing the window** |
| `scripts/shader_probe.c`, `scripts/light_probe.c` | the real fragment shaders run headless over known inputs, read back | **The bit-identical proof for a shader branch that is switched off** |

### Upstream already has a texture pack system, and the port's docs never mention it

**Confirmed, read from the vendored tree.** `tools/RecompOne/RecompOne.Runtime/Assets/`
holds:
- `AssetReplacerManager`, which loads `./packs/` as folders or zips at startup;
- `AssetPack` and `PackManifest`: `pack.json` with `id`, `name`, `author`,
  `version`, `priority`, a `game` id (strict by default), and lists of textures,
  CLUTs, texture rules and XA replacements;
- `TextureTile.Hash`: an FNV-64 hash of the texel indices plus a separate hash
  of only the CLUT entries those indices use;
- `TextureResolver`, `TextureDumper` and `TextureRegistry`;
- a Texture Inspector panel with tile and page dumping.

`GlCore.ResolveReplacement` binds the replacement as `uRepTex`/`uRepClut` in
`PrimFs`.

That is the right identity for a texture and the right home for replacement
images, and the remaster should use it rather than build a second one. **What has
not been measured is how it behaves on the port's own texture path.** That path
has three pieces:
- the anisotropic kernel (`0041`) in `decode()`;
- the texture-rectangle clamp and the mip atlas (`0060`);
- the fluid scroll (`0053`).

The replacement branch in `PrimFs` sits beside all three. **Inferred**: a
replaced texture skips the mip atlas and the rectangle clamp. That is the first
thing Phase 4 measures.

## Identity: what authored data attaches to

**Everything else in this document depends on this section, so it comes first.**
Authored data has to stay attached to the same thing across frames, sessions,
save files, the port's own settings, and differently mastered discs. Packet
addresses change every frame. LBAs change between masterings. Screen positions
change with the render scale and the aspect.

### The rule for what an authored file may hold

**Identifiers the game defines, and the author's own values. Never the game's
payload.** Allowed:
- area numbers, tile coordinates, table indices and model ids;
- content hashes;
- colours, positions, intensities and file names the author chose.

Never allowed: bytes, texels, vertices, text or any other copy of disc content.
A hash is a fingerprint, not a copy: it cannot be turned back into the texture
or the tile block it was taken from. This rule is what lets a pack sit in the
repository and be shared between players who each own the disc.

### The keys, and what breaks each one

| key | names | what breaks it | how the design survives it |
|---|---|---|---|
| **Game**: the disc serial `SLUS-00158`, as upstream's strict `PackGame.Id` | the whole pack | a different game, above all the US-boxed *King's Field II* (`SLUS-00255`) | already refused twice: by `DiscCheck.Validate` and by a strict pack game id |
| **Area**: the area byte at `0x8017E060` **plus an area fingerprint**, an FNV-64 of the 64,000-byte tile block **less each half's `+2`** and the first `0x600` bytes of the collision-shape block, **taken from the loader's source buffer** as `func_80017244` copies each into `0x801C8484` and `0x801D8484` — the disc's bytes, before the game rewrites any. `+2` is out because the game writes a moving footprint into it (**Confirmed**, see "Phase 1, the first slice"); the live block is out because doors and lifts rewrite `+3` and `+4` and a save keeps them (**Confirmed**, see "The fingerprint moved with the game's progress") | everything authored in that area | a different revision or region of the same game | the fingerprint is taken before the remaster's own edits and before the game's. **On a mismatch the area's layers are switched off whole, and the editor says why**; they are never half-applied |
| **Tile half**: `(area, x, z, lower \| upper)` | the mesh instance the tile draws, whole (a cave tile's floor, ceiling and rock together; see "A tile half is a whole mesh"); a face of it is `(tile half, mesh, face index)`; its collision cell; its light record (`+4 & 0x3F`) | the game rewriting tiles at run time: the drawbridge and the minecart are tiles, not models (see "The map is an 80x80 tile grid" in `GAME_INTERNALS.md`) | the key stays valid, but what it names can change under it. An entry may carry a condition on the tile's current model index, which is an index and not payload |
| **Tile mesh**: `(area, model index at half +0)` | every instance of that mesh in the area | nothing known. **Open**: where the area's model bank lives, and so whether two areas share a mesh | once the bank is found, a content hash of the mesh gives an identity across areas |
| **Model**: `ModelDraw.Model` (the model id), and for objects `(area, definition index at rec +0x6)` | "every creature of this kind", "every torch" | an MO morph changes a model's vertices, not its identity | a kind is the natural key for materials and for lights attached to a model |
| **Instance**: `(area, table, spawn ordinal)`, where the ordinal is the slot the area loader filled | one placed prop or creature | dynamic slots (drops, projectiles, respawns); saved state (a killed creature, a picked-up item) | **Inferred**, not measured, that static props land in the same slot on every load. Fallback: `(area, definition index, spawn position rounded to 64 units)`, matched to the nearest live record at load |
| **Texture**: upstream's `(index hash, CLUT hash, bpp, w, h)` | a piece of art, wherever VRAM puts it | (a) the key hashes the **polygon's UV bounds**, so one piece of art can have several keys; (b) the scrolling textures are re-uploaded every tick, so their content, and their hash, changes every tick; (c) a CLUT that cycles; (d) a region the GPU drew into is rejected as dirty; (e) a port patch that changes a palette (`MessageText` zeroes one) | (a) key a face on its `GteTexRect` rectangle, and measure whether the game uploads per texture or per page (**Open**); (b) key the fluid slots as `(area, fluid slot)`, the same slots `Reflections` already publishes as water; (c, e) a material keys on the **index hash alone**, with the CLUT hash as an optional narrowing |
| **Face**: `(mesh key, primitive index)` | one polygon of one mesh | nothing, provided the assembler can expose the index | only needed for per-face materials, late in the roadmap |

### What the keys survive by construction

- **Render scale and widescreen.** Every key is in world space or VRAM space, and
  VRAM is always the console's 1× (`0054`). Nothing is keyed on a screen position.
- **A mod or patch being toggled.** The area fingerprint is taken before edits.
  Materials key on the texel indices, not the palette, so a patch that zeroes a
  palette leaves the key alone. `kf2debug`'s noclip moves the player, not the map.
- **A different mastering.** No LBA, file offset or `fdat` sector appears in any
  key. The generated dispatch tables bake LBAs (see `PACKAGING.md`); the remaster
  must never read them.
- **A save file.** Tile, model, texture and area keys are all re-resolved when an
  area settles. Instance keys are the weak ones, and their edits are the ones that
  can leak into a save (see "Level editing").

**Forbidden as keys:** a packet address, an ordering-table position, a screen
position, a VRAM position on its own, an LBA, and a slot number for anything that
can be spawned.

### When an area has settled

**Confirmed in Phase 1, with two additions the design did not have.** A New Game
passed the floor test once *before* `fdat02` loaded, on the block the last area
left, so an area can settle only after an `fdat` module has loaded since the last
executable did. And the fingerprint must read the same twice 200 ms apart, so a
block still being copied in cannot settle.

**The loader is not the signal.** `func_8001689C` is called every frame with the
load inside a branch (see "The area loader looks like the right hook and is not"
in `PATCHES_AND_MODS.md`), and `OverlayLoadedEvent` fires before the tile block
is copied. The map patches wait for the invariant instead: the player stands on a
drawn half whose floor height matches their own Y exactly. The identity layer
uses the same test, plus the area byte changing. It settles on the first frame
both hold, and only then applies the area's layers. The fingerprint it settles on
is the one hashed as the loader copied the block in; only if that copy was never
seen does it fall back to the live block, and the probe line says `LIVE` when it
does.

### What has to be measured before the keys are trusted

1. **Instance stability.** Load an area twice, reload a save, and compare
   `slot → (definition index, position)` for the object and creature tables.
2. **The model bank's address**, for mesh identity across areas.
3. **Texture upload granularity.** Do `LoadImage` calls carry one texture each,
   or whole pages? That decides whether key (a) needs normalising.
4. **Which tile bytes the game rewrites at run time, and when.** The map already
   copies the block four times a second; the probe needs a diff.

## Architecture: five layers and the seams between them

**A new feature must be a new file, not an edit to five.** The port's patches have
so far each been a vertical slice: hooks, state, settings page, probe, shader.
That works for one feature and does not work for twenty, because each one edits
`GlCore`, `GlShaders`, `Program.cs` and a settings page. The remaster splits the
work into layers, and each layer has one owner.

```
  editor UI (patches/editor/)     picks, gizmos, inspectors, undo
        |  edits documents only
  data model (patches/remaster/)  packs, documents, layering, versions, file watch
        |  change events, on the game thread
  application (one IRemasterFeature per layer)
        |  writes side tables and uniforms, hooks through HookAttach
  render features (vendored runtime, 0071+)   one uniform block, shader terms
        |
  identity (patches/remaster/Identity.cs)     area, fingerprint, keys, resolvers
```

1. **Identity** (`patches/remaster/Identity.cs`). This layer:
   - detects when an area has settled and takes the fingerprint;
   - defines the key types;
   - resolves a world position to a tile half, a `ModelDraw` to an instance or a
     model, and a texture to its key through upstream's `TextureTile`.

   It reads guest memory and writes nothing.
2. **Data model** (`patches/remaster/Pack*.cs`). This layer:
   - reads and writes documents as JSON;
   - migrates versions and layers packs;
   - watches the pack directory. A change on disk is parsed on a worker thread
     and swapped in at the next `VSyncEvent`, the same marshal point `KF2_SHELL`
     uses for its light commands.
3. **Application**. One `IRemasterFeature` per layer:

   ```csharp
   interface IRemasterFeature
   {
       string Id { get; }                       // "lights", "surfaces", "level", ...
       bool Enabled { get; set; }               // KF2_REMASTER_<ID>, and a setting
       void Attach();                           // through HookAttach; read back IsCommitted
       void Detach();                           // puts back everything it changed
       void OnAreaSettled(AreaContext area);    // resolve keys, apply edits
       void OnDocumentChanged(DocumentChange c);
       void OnTick();  void OnFrame();
       void DrawInspector(Selection s);         // the editor's panel for this layer
       string Probe();                          // the KF2_REMASTER_PROBE line
   }
   ```

   The features are registered in one list in `patches/remaster/Remaster.cs`,
   which `Program.cs` installs **before `LoopPacing`**. `LoopPacing` has to stay
   last, because its post on stage 13 must run after the smoothers'. `Detach` is
   what makes off bit-identical: a feature that cannot put something back does not
   change it.
4. **Render features** (the vendored runtime). One new runtime file owns a single
   `RemasterUniforms` block: the material table, the light list, the fog colour
   and the sky. `GlCore` uploads it by generation, and the shaders read it. Once
   that block exists, a later render feature adds fields to it and a term to a
   shader, without editing `GlCore` again. Numbering follows the rule in
   `RECOMPONE_PATCHES.md`, where a new number is for a new mechanism:
   - widening the material ids and moving their table into the block **amends
     `0067`**;
   - authored lights are `0071`;
   - fog colour and the sky fill are `0073`;
   - normal and roughness maps are `0074`;
   - a GPU id buffer for picking would be `0075`, and only if it turns out to be
     needed.

   `snap`'s readback of the presented picture took `0069` and the hook order
   `0070`, so each planned number moved along from what this plan first said.
   `0071` is in (see "Phase 2, the first slice"); the block holds the light list
   only. **The material table did not move into it**: it stays `SurfaceMaterial`'s,
   widened by the `0067` amendment and uploaded as a 256×2 texture that the prim and
   reflection shaders both read, because 256 ids of reflectivity, F0, roughness and
   an emissive colour as uniform arrays would pass the fragment stage's uniform
   minimum (see "Phase 3, the first slice").

   All of it is **GL core only**; the 2.1 path and the software rasterizer ignore
   it, as they do `0048` and `0067`.
5. **Editor UI** (`patches/editor/`). ImGui panels, with gizmos drawn over
   `OutputView`. It reads the data model, the identity resolvers and the scene
   the walks publish. **It writes only documents**, never a side table or a
   uniform. That keeps undo, saving and live reload the same operation.

**The seam that matters most is between the editor and the application.** An
edit is a document change. The feature hears about it on the game thread and
re-applies. Nothing in the editor can put the game into a state the saved file
cannot reproduce.

## Data model and file format

### A remaster pack is an upstream asset pack with a `remaster/` directory

**Recommendation: do not invent a second pack format.** A remaster pack is a
folder or zip under `packs/`, with upstream's `pack.json` at its root. That gives
it for free:
- a game-id check;
- a priority order;
- enable and disable;
- loading from a zip;
- replacement textures and XA.

The port's own data sits next to `pack.json`, in `remaster/`, as one JSON file
**per layer per area**, so that a diff is small and two people editing different
areas never touch the same file:

```
packs/verdite-stone/
  pack.json                        upstream's manifest: id, name, author, version, game, priority
  textures/...                     upstream replacement images (the author's own art)
  remaster/
    pack.remaster.json             formatVersion, the features the pack uses, a gameplay-changing flag
    materials.json                 the material library, referred to by name
    textures.json                  per-texture: material, normal/roughness map, emissive
    areas/1/lights.json
    areas/1/surfaces.json          tile, mesh and model -> material
    areas/1/atmosphere.json        fog colour and curve, sky, light-record overrides
    areas/1/level.json             tile byte edits
    areas/1/props.json             port-drawn decorations (late)
```

Every area file carries its gate:

```json
{ "formatVersion": 1, "area": 1, "fingerprint": "9f3c1a0be4d27765" }
```

### The documents, by example

A material is named, and packs refer to it by name, so ids are allocated at load
time and two packs cannot collide on a number:

```json
// materials.json
{ "formatVersion": 1,
  "materials": {
    "polished-stone": { "reflectivity": 0.35, "f0": 0.04, "roughness": 0.3 },
    "wet-rock":       { "reflectivity": 0.2,  "f0": 0.03, "roughness": 0.6 },
    "brazier-coal":   { "emissive": [1.0, 0.45, 0.1], "emissiveStrength": 0.8 } } }
```

Surfaces assign materials, and the most specific key wins: a face of a half, the
whole half, a face of a mesh anywhere in the area, the whole mesh. A face list names
the mesh it was authored on and that mesh's hash, and applies only while both still
hold (see "Faces, picked from the frame"). A model is named by the table it comes
out of and its model id, since a creature and an object may share an id and not a
mesh:

```json
// areas/1/surfaces.json
{ "formatVersion": 1, "area": 1, "fingerprint": "9f3c1a0be4d27765",
  "tiles":  [ { "x": 35, "z": 36, "half": "upper", "material": "polished-stone" },
              { "x": 36, "z": 36, "half": "upper",
                "mesh": 1, "meshHash": "8613becbcde29491", "faces": { "1": "mirror" } } ],
  "meshes": [ { "mesh": 12, "meshHash": "0c41d9e2a7b3f865", "material": "wet-rock",
                "faces": { "3": "polished-stone" } } ],
  "models": [ { "kind": "object", "model": 486, "material": "brazier-coal" } ] }
```

Lights are world positions in the game's own units: a tile is 2048, a height step
is 128, and up is **−Y**. Built so far: `type`, `position`, `colour`, `intensity`,
`radius`, `direction`, `cone`, `flicker` and `enabled`; `falloff` is always the
smooth window, and `affects` and `shadow` are kept on a round trip and read by
nothing:

```json
// areas/1/lights.json
{ "formatVersion": 1, "area": 1, "fingerprint": "9f3c1a0be4d27765",
  "lights": [
    { "name": "hall brazier", "type": "point",
      "position": [72704, -15360, 73728], "colour": [1.0, 0.62, 0.3],
      "intensity": 1.2, "radius": 4096, "falloff": "smooth",
      "affects": "both", "shadow": "none",
      "flicker": { "amount": 0.15, "hz": 7 } },
    { "name": "shaft", "type": "spot", "position": [70000, -18000, 70000],
      "direction": [0, 1, 0], "cone": [20, 35], "colour": [0.7, 0.8, 1.0],
      "intensity": 0.6, "radius": 6144 } ] }
```

Textures use upstream's key string for the art, so a key read off
`TextureDumper` or the Inspector can be pasted in as it is:

```json
// textures.json
{ "formatVersion": 1,
  "textures": [
    { "index": "5b1e0c2a9d7f3e41", "material": "polished-stone",
      "normal": "maps/floor01_n.png", "roughness": "maps/floor01_r.png" } ] }
```

Atmosphere and level edits are covered in their own sections below.

### Versions, layers and removal

- **A version per file.** `formatVersion` is an integer, bumped when a field
  changes meaning. `PackMigrations.cs` holds one function per step. **Unknown
  fields are kept on a round trip**, so a newer pack opened in an older editor
  loses nothing. The editor always writes the current version.
- **Layering.** From bottom to top:
  1. the base game (no packs);
  2. packs, in upstream's priority order;
  3. the user's working pack, always on top.

  Each layer overrides the ones below it **per key**, not per file. An explicit
  `null` for a key removes it, which is how one pack turns off a light another
  pack placed.
- **The fingerprint gate applies per pack.** A pack whose area fingerprint does
  not match is off for that area only, and the Remaster packs page lists it with
  the reason.
- **Sharing is a zip of the working pack.** By the rule in "Identity" it contains
  no disc data, so it can be committed to this repository and handed to another
  player. Replacement images are the author's own art.

### Settings

- One **Remaster packs** page lists the packs found, whether each is enabled,
  which features each uses, and every load or fingerprint error.
- Each render feature gets one switch under Video ▸ Enhancements, beside the
  existing ones.
- Nothing is enabled on a first run, and the repository ships no pack switched on.
- A pack that changes collision or placement says so on that page, in plain
  words; see "Level editing".

## The editor

**Recommendation: in the game, in ImGui, not an external tool.** The scene exists
only in the running process:
- which tile the cursor is over;
- which model is where this tick;
- which texture a pixel came from;
- the camera block.

A tool outside the process would need all of that sent over a socket every frame,
and could still never show the result as the renderer draws it. In the game,
live reload comes free, and so do `PanelManager`, the map panels, the frame
viewer and `OutputView`.

**External tools keep the jobs they are good at.** The JSON is meant to be edited
by hand and is watched, so a text editor is a first-class tool. Images are drawn
in an image editor. A mesh importer for port-drawn props, if one is ever built,
is a command-line converter.

### Modes

- **Play.** The editor is closed and costs one bool a frame.
- **Edit.** The world is paused through `FramePacing.PauseWhen`, the mechanism the
  full-screen map already uses. Stage gating holds and the renderer keeps
  drawing, so lights and materials update live on a frozen scene. The editor
  camera takes over, and the panels open.

  **Shift+E** is proposed as the hotkey. It has to be checked against
  `KeyLayout` and the existing Shift+P, Shift+F and Shift+M, M and N.

### Picking

- **Tiles:** the faces under the cursor, from the frame's own triangles. While the
  editor is open every sealed packet is recorded with its face key, its screen
  corners and its depths, and a click takes the nearest, with every other face
  within the coplanar tolerance of it. That is what the depth buffer drew, walls
  and ceilings included. It replaced a ray through the 80×80 grid, which could see
  only floors (see "Faces, picked from the frame").
- **Models:** the same triangles. A model's are recorded with the kind and model
  id the object walk was submitting, so a click whose nearest triangle is a model's
  selects that model (`model:1:object:486`) rather than refusing. Instances are not
  told apart: a material on a model is on every draw of it in the area.
- **Textures:** on a click, a one-frame `FrameCapture`, which already answers
  which GP0 command and owner routine drew a pixel. The command gives the page,
  the CLUT and the UVs, and from those upstream's key. It is only needed on a
  click, so its cost does not matter.
- **The map panel** doubles as a top-down tile picker: it already reads all ten
  bytes under the cursor.
- **No GPU id buffer at first.** It would be a render-target attachment and a
  per-triangle id, which is a new runtime mechanism (`0075`). The CPU paths
  answer everything the first phases need.

### Gizmos

Gizmos are a move handle on each of the three axes, a radius sphere and a
direction arrow for a spot light. They are drawn in the ImGui draw list over
`OutputView`'s rectangle and projected the same way the AO and SSR passes
reconstruct positions: the view matrix at `0x80192E18`, H, and OFX/OFY. Snapping
is to 128 units in Y, the height step, and to tile centres in X and Z.

### The editor camera

**`Stage13.ViewOverride`.** Stage 13 is C# (`patches/Stage13.cs`), so the view
a frame is drawn from is a value: set a `Camera` and every frame is drawn from it,
with no camera block to save and put back. See "Drawing the frame from another
camera" in `docs/PATCHES_AND_MODS.md`.

**The visibility grid follows, measured.** The 24×24 window is built by the game's
own `func_8002D3A8`, which reads its eye from the camera block and nothing else of
the player's. So the grid is right for a free eye by construction. The `view` shell
verb tests it: the grid built from an override camera is byte-identical to the grid
built with the player standing there, with the player 3 to 20 tiles away in six
directions. (`patches/CullGrid.cs` was not the route: it is off by default and does
not match the game's build.)

**What does not follow is the arm and the object walk**: both read the player's
position directly, so from far away they may place or cull things by where the
player is. That is the first thing to look at in a frame drawn from an override.
`goto` plus the game's own camera remains the fallback, and it is enough for
Phases 1–6.

**A second view drawn beside the game's**, such as a picture-in-picture preview or
a shadow map's light view, is a `ScenePass` around `Stage13.DrawScene(c, mem,
camera)`: the pass points the frame at a table and arena of its own and puts back
everything any pass moves. See "A pass of the port's own" in
`docs/PATCHES_AND_MODS.md`.

### Undo, save, autosave

- **Undo** is a stack of document diffs, each able to apply and revert, kept per
  session. An edit made through a gizmo drag is one entry, not one per frame.
- **Save** is explicit, and writes the working pack's files.
- **Autosave** writes a `.autosave` file next to each changed document every 30
  seconds. On startup, the editor offers to restore it.

### Driving it without a window

New `KF2_SHELL` verbs, also exposed through `mcp/`, so an agent can run every
measurement in the roadmap:

```
edit on|off                        enter or leave Edit mode (pause, editor camera)
select <key>                       select by key: tile:1:35:36:upper, model:41, tex:5b1e..., light:1:"hall brazier"
set <key> <field> <value>          change one field through the same undo stack the panels use
pack save|reload|list              the working pack
snap [hash|PATH] [after N] [buffer Y|any]   the presented picture: its hash, what changed, a PNG
```

**`snap` is the tool the whole "off is bit-identical" rule depends on.** It is
built from what "Getting pixels out without a screenshot" in `DEVELOPMENT.md`
already does: the presented target read back from a `DrawOTag` post and written
with `PngWriter`. With the world paused and the view pinned by `goto x y z yaw
pitch`, two runs give comparable frames.

## Lighting: authored lights beside the game's own

**The game's lighting is a known chain, and the port already rebuilds it per
pixel.** A packet's colour is:

    RGBC * (BK + LCM * max(0, LLM · N))

followed by the depth cue, a darkening towards the far colour, which is 0 here.
Map tiles take one `NormalColorCol` per face, so **the depth cue is all of a
tile's shading**. `0048` records the chain's inputs per corner and `shade8`
evaluates it per pixel; `light_probe.c` measured the shader against the formula
with a worst difference of 0. See "Per-pixel lighting" in `RENDERING.md`.

### Recommendation: forward, inside `shade8`

**An authored light is one more term in the lit colour, added before the depth
cue and before the texture is modulated.** So:
- it is fogged by the game's own curve;
- it is textured exactly as the game's own light is;
- it saturates where the game's light saturates.

Every other placement gets at least one of those three wrong.

- **Position per fragment:** the view position rebuilt from the recovered depth,
  exactly as `NormalFs` rebuilds it, then turned into world space with the
  camera rotation and position `AoWorld` already uploads. No new vertex
  attribute.
- **Normal per fragment:** the plane from the view position's screen
  derivatives, as `0058` takes it. It is exact for the flat faces the tiles are.
  Gouraud normals for models are a later refinement: the light-dot record would
  also carry the normal.
- **The light list** is culled on the CPU against the camera and the 24×24 window
  every frame, capped at 16, and sent in `RemasterUniforms` by generation, so an
  unchanged list is not re-sent.
- **Receivers:** tiles and models. The first-person arm and anything placed in
  view space (a matrix of 0 in the submitter) are left out unless a light is
  flagged to reach them, because their positions are not in world space.
- **Off is a branch on a count of zero**, so the shader's output with no lights
  is the output it has today. That is proved headless: `light_probe.c` run with
  and without the uniform block, worst difference 0.

### Why not deferred

The surface buffer (`0067`) already holds a normal, a depth and a material at
every pixel, so deferred looks free. **It is not, because the colour it would
relight is final.** The texture has already been multiplied by the lit colour,
clamped and fogged, and water and the other translucents are drawn over it. A
deferred light would have to divide the game's lit term back out per pixel, and
that term saturates. Deferred stays the right place for terms that apply to the
finished picture: a specular highlight along the SSR path, light shafts, bloom.

### Shadows

- **None in the first version.** An authored light lights everything inside its
  radius, which is how the game's own light records behave.
- **Then a march through the tile grid.** `AoWorld` already marches the 80×80 grid
  in a shader for occlusion. A march from the fragment towards the light through
  the same texture gives whole-tile walls, with creatures and props casting
  nothing. It is coarse, but it is a known cost.
- **Shadow maps are pushed back.** They need the scene drawn from each light. The
  mirrored walk alone costs 1.09–1.12 ms of CPU a frame, measured under
  "Planar reflections", and the port is CPU-bound, so even one shadowed light
  would cost a planar reflection's worth of frame time. If a static shadow map
  per area is ever wanted, it should be taken once when the area settles, never
  per frame.

### The game's own light and fog, authored through the game

**The cheapest and most faithful remaster edit is to the game's own lighting
data.** Stage 1 copies the area's 80 light records from `0x800679A0` into
`0x801930F0` every frame; `TintHold`'s C# stage 1 does the copy (see "The tints
strobed between ticks" in `PATCHES_AND_MODS.md`). Each 104-byte record carries
the light matrix, the colour matrix, the back colour and the depth cue, and
`func_80031950` picks one per tile by `+4 & 0x3F`.

An override applied right after that copy runs through the game's own code, and
through `0048` unchanged, so it is exact whether per-pixel lighting is on or off.
**Inferred**: nothing but rendering reads those records. That has to be confirmed
with `scripts/find_writers.py` and a read census before an override ships.

Fog colour is the GTE's far colour, measured at 0, which is why the fog darkens
to black rather than fading to a colour. A fog colour becomes one term in
`shade8`: `mix` towards a colour instead of a multiply towards black.

## Level editing: what can be edited and what can only be decorated

**The split is whether the game's own logic reads it.** If the game reads it, an
edit changes gameplay, and it is a level edit. If only the renderer reads it, it
is decoration.

### Edited: the tile bytes, because the game reads them

The 80×80 block at `0x801C8484`, applied when the area settles, after the
fingerprint is taken, and applied again whenever the area is entered.

| byte of a half | what | read by |
|---|---|---|
| `+0` | model index; `0xFF` empty, drawn below 240 | `func_80031B1C` (the renderer), `CullGrid` |
| `+1` | height; the floor is `-(h) << 7` | `func_80031B1C`, the floor queries `func_8002B6B4` and `func_8002C3A8` |
| `+2` | collision flags, `& 0xFC` | `func_8002C700` |
| `+3` | collision-shape index into `0x801D8484` | `func_8002B7D0` |
| `+4` | flags: bit `0x80` stops the visibility flood; the low six bits pick the light record | `CullGrid`, `func_80031950` |

Limits that follow from the format and cannot be designed away:
- **Heights move in steps of 128 units.**
- **A tile can only show a mesh the area already has.**
- **Collision shapes come from a 0x600-byte block**, so a new shape is a slot
  in that block, if one is free.
- **The game's own tile rewrites win.** A door, the drawbridge or the minecart
  rewrites its tiles at run time, so an edit to one of those tiles is undone by
  the game. The editor flags such tiles from the rewrite census.
- **Open**: whether any of the tile block is written into a save, or whether it
  is always re-read from the disc on an area load. Until that is measured, level
  edits are marked gameplay-changing.

The test for an edit is the invariant the map already prints. Stand on the edited
tile with `goto`; `state` must give a floor gap of 0 at the new height. Walk into
an edited wall with `press`; the position must not cross it.

### Edited with a warning: placement of objects and creatures

Objects at `rec+0x14` in the table at `0x80177714`; creatures at `rec+0x2C` in the
table at `0x8016C544`. **`func_800492B8` packs the 200-slot creature table into
the save buffer** when the player saves. A moved creature is therefore written
into the player's save, where it outlives the pack and stays after the pack is
switched off. That is the one kind of edit that can change a player's game
permanently. It is last in the roadmap, opt-in per pack, and labelled on the
Remaster packs page.

### Decorated: everything only the renderer reads

- materials, lights, fog, sky, and the light-record overrides;
- **port-drawn props**: meshes the author supplies, submitted through
  `PolyAssembler` into the frame's ordering table, so they get depth, fog,
  per-pixel lighting and reflections the same way the game's own models do.
  They have no collision, and are said to have none. This is the only way to
  add geometry, and it is late in the roadmap.

### Pushed back

- **Replacing creature meshes.** A creature is an MO morph, a base TMD plus
  packed vertex deltas per clip (see "The model pipeline has no skeleton" in
  `GAME_INTERNALS.md`). A replacement would have to carry matching morph targets
  for every clip. That is a modelling pipeline, not an authoring tool.
- **New walkable geometry** beyond what tiles and collision shapes can express.
- **Lifting the 24-tile draw window.** Already measured as binding and barely
  worth it; see "Is the 24-tile window worth lifting?" in `WIDESCREEN.md`.

## Rules every phase keeps

- **Off is bit-identical, and it is proved three ways**, depending on what the
  feature touches:
  1. `snap … hash`, feature off, against a build without the feature, at a pinned
     view: the world paused, `goto x y z yaw pitch`, at `KF2_AUTOSTART=new` and
     at `KF2_AUTOSTART=2`;
  2. `shader_probe.c` or `light_probe.c` for any shader branch;
  3. `KF2_*=verify` for any C# routine a feature adds to (`KF2_POLYASM`,
     `KF2_TILEWALK`, `KF2_MODELWALK`), with 0 RAM, register and GTE mismatches.
- **144.0 fps drawn at 20.0 ticks/s**, measured with
  `KF2_FPS=144 KF2_FPS_PROBE=1 KF2_PRESENT_PROBE=1`. Each feature's cost is taken
  from the frame profiler, on and off. GPU time comes from a frame capture's
  `GL_TIME_ELAPSED` queries, since frame rate cannot see GPU cost on a CPU-bound
  port.
- **Features ship off; no pack is enabled by default; the repository ships none
  switched on.** The remaster never breaks faithfulness by default.
- **Every feature has a `KF2_REMASTER_<ID>` switch and a probe line**, both listed
  in `ENV_VARS.md`.
- **Hooks attach through `HookAttach`** and read back `IsCommitted`, as every patch
  does (see "A registration is not a hook" in `PATCHES_AND_MODS.md`).

## The phased roadmap

Each phase is usable on its own. For each one this lists what ships, what it
depends on, the risks, the counter that proves the mechanism, and what the user
has to look at, because nobody else can.

### Phase 1: an authored material on a floor, end to end

**The smallest slice that exercises every layer:** one material placed in the
editor, saved, reloaded and rendered. A material, not a light, because the
reflection pass already reads `SurfaceMaterial` and `GtePacketDepth.Rec.Material`
already exists, so **no new runtime patch is needed**. The whole slice is
port-side. (Wrong by one amendment to `0067`; see "Phase 1, the first slice".)

**Status:** [x] done and measured, [~] done in part, [ ] not started.

- **Ships:**
  - [x] `Identity` with area, fingerprint and tile half only;
  - [x] `TileWalk` publishing the half it is assembling (`x, z, half`), the way
    `ModelWalk.SetSubmit` publishes a model;
  - [x] `PolyAssemblerDepth.SealDepth` writing `Rec.Material` from a table the
    surfaces feature fills;
  - [x] `SurfaceMaterial.Reflectivity`/`F0` filled from `materials.json`;
  - [x] the pack loader (the working pack only);
  - [~] a minimal editor: tile pick by ray and from the map panel, a material
    inspector, save, reload, live reload, undo -- all in, driven over the shell
    and used by hand to place the first mirror floor;
  - [x] the `edit`, `select`, `set`, `pack` and `snap` shell verbs (`snap` in the
    second slice, reading the presented target through `0069`);
  - [x] the Remaster packs settings page, and the switch under Video ▸
    Enhancements (the second slice).
- **Depends on:** reflections being switched on (`KF2_SSR=1`), since that is the
  only reader of a material today.
- **Risks:**
  - the eight material ids. The slice needs one; if it needs more, the `0067`
    amendment moves into this phase;
  - SSR's water assumptions (the fog curve, the sky fallback) may look wrong on
    a floor. That is for the eye to judge, and it is useful to learn early.
- **Mechanism measured by:**
  - [x] `SurfaceMaterial.FromPacket` and `ByMaterial[id]` rising for the authored id;
  - [~] the SSR readback's reflective share with the tile in view -- the id shows
    in the readback's map, but the only tile tried was the water surface, so the
    share could not move;
  - [x] the same tile key resolving after a `warp` out and back, after `load 2`, and
    after a restart;
  - [x] the fingerprint identical across those -- once `+2` was left out of it;
  - [x] `snap` identical to baseline with the pack disabled (the second slice);
  - [x] `KF2_POLYASM=verify` and `KF2_TILEWALK=verify` clean;
  - [x] 144.0 at 20.0.
- **You look at** (the first judged: a dry tile at reflectivity 1 reads as a
  mirror, placed correctly):
  - whether the floor reads as polished stone or as a mirror;
  - whether the reflection's fog and sky fallback look wrong on something that
    is not water;
  - whether the editor is usable: picking, the inspector, the save round trip;
  - [ ] a floor at a partial reflectivity, and a larger area of floor.
- **Cost:** one table read per sealed tile packet while on, one bool while off.

### Phase 1, the first slice

**What is in.** Everything in the Ships list above except `snap`, the Remaster
packs settings page and the camera-free parts of the editor's gizmos:

- `patches/remaster/Identity.cs` — area, fingerprint, the settle test, `TileKey`
  (`tile:A:X:Z:lower|upper`), record address to key and back.
- `TileWalk.CurrentRecord` — the half `func_80031950` is assembling, set around the
  assembler call. `PolyAssembler.TileMaterial` is what `Surfaces` resolved for it,
  and `SealDepth` writes it into `GtePacketDepth.Rec.Material` for every packet it
  seals, clipped fans included. A material therefore needs the C# half
  (`KF2_TILEWALK_TILE=0` authors nothing).
- `patches/remaster/Pack.cs` — the working pack (`packs/working`, or
  `KF2_REMASTER_PACK`), `materials.json` and `areas/<n>/surfaces.json` kept as JSON
  trees so unknown fields survive, upstream's `pack.json` written once, a
  `FileSystemWatcher` whose parse is swapped in at the next VSync, and one undo stack.
- `patches/remaster/Surfaces.cs` — the one `IRemasterFeature` so far: names to ids
  from `SurfaceMaterial.FirstAuthored`, the fingerprint gate, an 80×80×2 id table.
- `patches/remaster/Pick.cs` — the game's projection both ways: the view matrix at
  `0x80192E18` less the camera (whose 16-bit X and Z are unwrapped against the
  player's), and `GteDepth.ProjH`/`ProjCx`/`ProjCy`. A floor ray is walked cell by
  cell through the grid; walls are not read, so a ray can pass through one.
- `patches/remaster/Editor.cs` — Shift+E; pauses the world; picks by a click on the
  picture, by right-click in the docked map, or as the player's tile; outlines the
  selection; a material library with reflectivity and F0 sliders (one undo entry per
  drag); Save, Reload, Undo, Redo.
- `patches/remaster/Shell.cs` — `edit`, `select`, `set`, `pack`, `remaster` on
  `KF2_SHELL`, through the same calls the panel makes.

**The runtime needed one change after all, an amendment to `0067`.** `NormalFs`
decided opacity from the id (`vM < 1.5`), so any authored id on an opaque floor
would have taken that floor out of the occlusion pass's normals. Opacity now
travels with the triangle; every existing id is drawn as before. The design's claim
that Phase 1 needs no runtime patch was wrong by exactly this.

**Measured** (`KF2_AUTOSTART=new`, `KF2_SSR=1`, area 0 in `fdat02`):
- The authored id reaches the classifier and the surface buffer:
  `SurfaceMaterial.ByMaterial[4]` and `FromPacket` rise together (4,032 in one
  window), about 4,600 authored packets a second for two tiles in view.
- The pick and the projection agree with the picture: `select pick 160 200` chose
  tile 37,45, and the SSR readback's material map then showed the new id in the
  cells covering game pixels 115-169 by 195-210. Picks follow the player's heading.
- Tile 37,45 is **the water surface itself** — the water is a tile, drawn blended by
  the tile assembler — so a packet material there replaces `Water` and the pixels
  stay reflective (the readback's reflective share did not move, 36.3%). It is also
  the amendment's test: a blended tile with an authored id stays translucent.
- **The first fingerprint was too strict.** Area 0 read `75069e…` from a New Game
  and `fab65f…` entered from save slot 2. The probe's diff: 32 empty lower halves
  (model `FF`) in two 4×4 blocks, `+2` bit `0x04` set in one and cleared in the
  other, nothing else different. Something four tiles square moves between the two
  saves and stamps its footprint into the collision flags; the ship by the shore is
  the obvious candidate (**Inferred**). With `+2` left out, area 0 reads
  `3f7d7b45edcbda59` from both, and area 1 `49930d41f830e0a3` across two loads and
  an auto-reload.
- **The fingerprint moved with the game's progress.** Reported from play: area 1
  refused a pack authored against `b6770a…` once the player had been elsewhere,
  reading `5056f1…`. Measured from slot 2: area 1 read `49930d…` on entry and
  `49787d…` after leaving for area 2 and coming back, and flipped back again on a
  reload. The probe's diff, with the ignored `+2` taken out: 12 upper halves at
  10-13 × 47-49, `+3` (the collision shape) `75` → `32`/`33` and `+4` losing bit
  `0x40`, and no collision-shape byte. The game rewrites a door's or a lift's tiles
  as it plays, and that state travels with the save, so **no live reading is the
  area's identity**. The fingerprint is now taken from `func_8001689C`'s source
  buffer as `func_80017244` copies it (a pre-hook matching the destination and the
  word count). Measured after: area 1 `be64c93e02071c09` on entry, after areas 2
  and 3, and after reloads from slots 1, 3 and 2, while the live block went on
  flipping; area 0 `58d4e515d1aea80a`, area 2 `f6f8cec2c5dc7135`, area 3
  `370ef222844b0ee9`. The values below are the old live readings and no longer
  match anything.
- A pack authored against the old fingerprint was refused whole, with the reason on
  the panel and in `remaster`; editing the file's fingerprint by hand applied it
  through the watcher with no restart.
- A hand edit adding a material with an unknown field (`roughness`) and a root field
  reloaded live and survived the next save.
- `KF2_TILEWALK=verify KF2_POLYASM=verify` with materials applied: 1,297 reports,
  all 0 RAM, register and GTE mismatches.
- 144.0 fps drawn at 20.0 ticks/s, `[present] wide 288`, vertex map 97.1% hit, with
  the remaster on. `set remaster off` in the same run: sealed packets stop, the
  authored id's count stops growing.
- **Off costs one test a frame** (`Host.Frame` returns before reading anything) and
  one byte store per sealed packet, which writes the `None` the record always held.

**Not measured, and why** (at the time; the second slice built `snap`). `snap` is not built: `GpuHle.Backend.ReadVram` reads 1×
VRAM, and the reflection pass composites at present, after VRAM, so a VRAM hash
cannot see what a material changes. `snap` has to read the presented target, which
is a runtime hook (`GlCore.PresentDisplay`) — next. Until then "off is
bit-identical" rests on the record holding `None` with the switch off and on the
amendment keeping every existing id's opacity, both read from the code.

**Looked at.** One dry floor tile in area 0, set to reflectivity 1 through the
editor, judged good as a first step: it reads as a mirror of the doorway and the
wall above it, in the right place and the right way up. The editor was used end to
end to get there (pick, material, sliders). Still to judge: a floor at a *partial*
reflectivity (does it read as polished stone), and a larger area of floor. The
same screenshot shows a speckled fringe along the reflected wall edge; that is the
reflection pass's march, not the material, and is in `docs/TODO.md`'s open
questions.

**Next.** Taken in the second slice, below: `snap`, the settings, and picks that
stop.

### Phase 1, the second slice

**What is in.** The three things the first slice left for next:

- **`snap`** (`patches/remaster/Snap.cs`, runtime `0069`). `snap [hash | PATH.png]
  [after N] [buffer Y|any]` reads the picture the window is drawn from -- after the
  occlusion, the reflections and any post shader, at the render scale -- hashes it
  (SHA-256, the first sixteen hex digits), counts the pixels that differ from the
  previous snap with their largest channel difference and bounding rectangle, and
  writes a PNG when given a path. It waits two presents by default, so an edit made
  by the command before it has been drawn and presented. The reply is sent from the
  present that was read, so `AgentServer` hands `snap` its reply rather than
  answering it at the VSync drain.
- **The settings.** Video ▸ Enhancements gains *Remaster packs*, the saved switch
  (`kf2.remaster.on`, the same key the editor's checkbox writes; `KF2_REMASTER`
  still overrides it). Under it a *Remaster packs* heading lists the working pack:
  its root, its materials, every area it holds with the fingerprint that area was
  authored against, and for the area now loaded whether it applies or why not.
  Only the working pack exists, so the list is one entry; layering is Phase 7's.
- **Picks that stop** (since replaced by a pick from the frame's triangles; see
  "Faces, picked from the frame"). `Pick.Floor` now stops a ray at a tile with no drawn floor
  (rock) and at a tile whose lowest floor is above the ray where it enters (a step
  up), and says which: `select pick` answers `no floor under that pixel: a step up
  at 44,47`. The eye's own tile is never a stop.

**The wall flag is not a wall.** The plan was to stop at `+4` bit `0x80`, which
`AoWorld` and `CullGrid` read as the tile that stops the visibility flood. Measured
over the 12×12 tiles around the New Game spawn in area 0: **every one** of them
carries it, on both halves, water, shore and sea floor alike. Stopping on it
stopped every pick at the tile next to the player. So the grid has no wall a ray
can read here, only floors, and a real wall -- one standing on a floor, with floor
beyond it at the same height -- still lets a pick through. The depth buffer is the
exact answer (the picture's own nearest surface under the cursor), and it is
where picking goes if this is not enough; `AoWorld`'s comment calling the bit a
wall is **Open** for the same reason.

**Measured** (`KF2_AUTOSTART=new`, `KF2_SSR=1`, area 0 in `fdat02`, 144 fps,
render scale 5, 16:9: a 2140×1200 picture):
- **A paused frame is repeatable.** Three snaps in a row, same hash. The two
  display buffers are not: at `0,0` and `0,240` the same paused frame differed in
  4 pixels by up to 3 levels, so a snap takes the buffer the last one took (the one
  at row 0 first) and says which in `buffer`.
- **Off is bit-identical.** With nothing authored, `set remaster off` and `set
  remaster on` gave the same hash (`8661b58c3cd089e9`). With a mirror on the tile
  a pick chose, off went back to the unauthored hash and on to the authored one,
  both ways round.
- **A pick lands where it was clicked.** `select pick 140 238` chose
  `tile:0:35:47:upper`; a mirror there changed 3,881 pixels (0.15%) by up to 3
  levels, all inside render pixels 900-1069 by 1151-1199 -- game pixels 126-160
  by 230-240, around the click. A mirror on the water tile under `200 238`
  changed 46,874 pixels in a rectangle round that one.
- **A mirror can change nothing, and that is not the pack failing.** In one view
  straight after the area loaded the same mirror on 35,47 moved no pixel at all,
  while its packets rose (2,072 in two seconds, `ByMaterial[4]` with them). A
  floor at the bottom edge reflects upwards and out of the picture; a ray that
  finds nothing and crosses no background pixel adds nothing. The SSR readback
  (`KF2_SSR_PROBE=1`) is what tells "not applied" from "nothing to reflect".
- 144.0 fps drawn at 20.0 ticks/s, `[present] wide 288, plain 0, vram fallback 0`,
  with the remaster on and after a dozen snaps. **Off costs one null test a
  present**; a snap costs one `glReadPixels` of the picture and a hash, once.

**Not judged.** The settings page and the editor's new "No floor there" line have
not been looked at. The pick's stops are checked against the grid only: whether a
click on a cliff face or a bank reads as "nothing picked" rather than "wrong tile"
is for the eye.

**Next.** The texture-key census (Phase 4) can start independently; Phase 2's
light term is next on the main line and needs nothing more from Phase 1.

### A tile half is a whole mesh, and a face is the key under it

**Reported from play: a material on a floor tile put the ceiling above it in the
mirror too.** Nothing is wrong with the pick. A tile half draws one mesh, and in a
cave that mesh is the tile's whole column: floor, ceiling and the rock between.
`TileWalk` sets the half's material around the assembler call, so every packet the
mesh makes carries it. The key table's "one floor or ceiling surface" was never
what the code did.

**Neither facing nor texture separates them.** Faceted rock points every way, so
a normal threshold cuts it at arbitrary seams, and nothing promises that a floor
and a wall use different textures. The face itself does: a mesh is a fixed list
of polygons, and `func_80030540`, `func_8002FECC` and the clipped fans all walk it
in order, so `(tile half, mesh, face index)` names one polygon on every path.

**The subdivider keeps the order.** Read from `func_80030C94`: it walks the
source faces in order and writes each one's pieces contiguously. A quad or a
triangle (`0x2C`, `0x2E`, `0x24`, `0x26`) becomes four, and anything else is
copied as one. So an output face maps back to its source face by counting.

**Measured** (`KF2_FACE_PROBE=1`, `patches/remaster/FaceProbe.cs`; `KF2_AUTOSTART=2`, area 1 in
`fdat05`, `KF2_SSR=1`, eight cameras through `view`). Every tile face was given one
of the ids 4-7 by a hash of its key, and the reflection probe's readback holds the
id per pixel:
- **The mapping.** 763k source faces over 15 meshes, 11.9M output vertices: 0
  count, command, CLUT or tpage mismatches. Every output vertex lies inside its
  source face's box, and every source corner reappears in its four pieces. No
  copied (non-splitting) face occurred, so that branch is read from the code only.
- **A pick from the frame's own triangles.** Every sealed packet was recorded
  with its key, its screen corners and its depths. The nearest triangle under a
  point was then compared with the GPU's id at that pixel. At face centroids it
  agreed 100% once the view had settled. On a 6-pixel grid it agreed 98-100%, and
  almost every disagreement lay within 2 px of the picked triangle's edge.
- **The one interior disagreement is overlapping geometry.** In one view, a band
  near the camera went to a different face than the nearest one. Two neighbouring
  tiles' meshes overlap there: face 1 of `801CF667` and face 1 of `801CF671` cover
  the same pixels 1 unit of depth apart. With the depth tolerance off
  (`KF2_ZBUFFER_BIAS=0 KF2_ZBUFFER_SLOPE=0`) the band stays, so the depth test's
  own fight decides it, not the tolerance.

**What follows for the design.** Key materials by face. Pick by the frame's
triangles, and when faces lie within the depth tolerance under the cursor, select
all of them: which one wins there changes per pixel and per angle, so an author
cannot see one without the other anyway.

### Faces, picked from the frame

**What is in.** Materials are authored per face, and a click picks faces:

- `patches/remaster/Faces.cs` -- the face being assembled (`Faces.Enter`, from
  both of `PolyAssembler`'s face loops), the subdivider's output mapped back by
  counting (`Faces.Subdivided`, refused whole if the count does not come out), the
  frame's triangles recorded at `SealDepth` while the editor is open, the pick,
  and the meshes: faces, a structural hash, and the grow queries.
- `Surfaces` resolves four levels per face: the half's face list, the whole half,
  the mesh's face list, the whole mesh. `TileWalk` asks for the half
  (`Surfaces.EnterHalf`) and `Faces` for each face, and only while something
  below the whole half is authored (`Surfaces.PerFace`).
- `Pack` keeps face lists on tile entries and area-wide rules under `meshes`, each
  with `mesh` and `meshHash`. A face edit is one undo entry, undone by putting the
  area's document back.
- The editor picks faces on a click (Shift+click adds or removes), grows a
  selection (*Connected*: joined by an edge and the same texture; *Same texture*;
  *Whole mesh*), selects the whole half, assigns at the half or, with *Every tile
  with this mesh*, at the mesh, and tints the selection's triangles over the
  picture. The map panel and the player's tile still select a whole half.
- Shell: `select pick GX GY [add]`, `select faces F,F,...`,
  `select grow connected|texture|mesh`, `set selected material NAME|none [tile|mesh]`;
  a selection's reply carries `sealedIds`, what the last frame sealed each of the
  half's faces with.
- `Pick.cs` is gone, and with it the second slice's picks that stop at rock.

**The mesh table pointer moves within a frame.** `0x8018E19C` points at the tile
meshes only while the tile walk runs; the object walk repoints it at the models',
so read at VSync it named nonsense (mesh 1's face count `0x00100008`). The walk
notes it for every half (`Faces.TileTable`), meshes are read from that, and
`Surfaces` re-applies when it moves. Its `+4` word is the far-model limit, not a
count.

**The mesh hash** is the face count and each face's command, length and corners:
the structure a face index means, not its texture or where its vertices are, so
a scrolling UV cannot move it. A face list whose mesh no longer hashes the same is
dropped whole, and the editor says so.

**Measured** (`KF2_AUTOSTART=2`, area 1 in `fdat05`, a scratch pack, `KF2_SSR=1`):
- A click on the floor in front of the player picked `tile:1:36:36:upper:1`
  (mesh 1, four faces). Authored as a mirror, the last frame sealed face 1 with id 4
  and faces 0, 2 and 3 with 0, and the picture changed in rows 559-1117 of 1200
  only. The same material on the whole half sealed all four faces and reached row
  35, the ceiling. Undo returned the face-only picture to the same hash.
- *Connected* grew face 1 to faces 1 and 3.
- A mesh rule on face 1 of mesh 1 reached the two other mesh-1 tiles in view and
  left a mesh-27 tile alone.
- After a restart the saved pack sealed the same ids. A hand-edited `meshHash`
  dropped that face list (`meshRefused: 1`) through the watcher, and the mesh rule
  still covered the face.
- The pick against the GPU (`KF2_FACE_PROBE=1`, nine cameras through `view`):
  about 20,200 grid samples, no interior disagreement, and 46 (0.23%) within 2 px
  of a triangle edge. Selecting the coplanar faces too took the overlapping view
  from 56 disagreements to 3.
- `KF2_TILEWALK=verify KF2_POLYASM=verify` with face lists applied: 162 reports, all
  0 RAM, register and GTE mismatches. 144.0 fps drawn at 20.0 ticks/s,
  `[present] wide 288, plain 0, vram fallback 0`.
- **Cost:** nothing authored below the half, one bool per face. Authored, a few
  array reads per face. Recording runs only while the editor or the probe is open.

**Not judged.** The editor's new controls and the selection tint have not been
looked at, and neither has whether *Connected* grows to what an author means on
faceted rock.

**Every "click" above was the shell's `select pick`, and the window's own click
never worked.** Reported from play: neither *Pick on the picture* nor *Place on the
picture* did anything. The editor gated a click on `!io.WantCaptureMouse`, but the
picture is drawn inside the Output panel, an ImGui window, so ImGui wants the mouse
whenever the pointer is over it: the gate was shut exactly where it should open.
`OutputView.Hovered` (`0029`, amended) is the picture's own `IsItemHovered`, which
is also false when another panel or a popup is in front of it. The click through
the window is **not yet confirmed by hand**.

**Typing reached the hotkeys.** A key typed into a text field (a material's name)
also arrives on the `KeyboardEvent` bus, so an `m` opened the map. The port's
hotkeys now ask `patches/HotkeyGate.cs`: Shift+E, Shift+P and Shift+F wait while
ImGui has a text field focused, and M, N and the mouse-capture key also wait while
the editor is open. `mods/kf2debug`'s F-keys are left alone, since nothing types
them.

### Phase 2: authored point and spot lights (`0071`)

- **Ships:**
  - [x] `RemasterUniforms` and the light list;
  - [x] the light term in `shade8` (core `PrimFs`);
  - [~] the light gizmo and inspector: a dot and a reach ring per light, a drag
    across the screen at the light's depth (Shift: up and down in height steps),
    and the inspector; no axis handles;
  - [x] `lights.json`;
  - [x] flicker, evaluated on the world tick so it holds with the world;
  - [~] a probe counting lights culled, uploaded and lit batches (not fragments).
- **Depends on:** Phase 1's pack, editor and `snap`; per-pixel lighting on
  (`0048`), since the term lives in `shade8`.
- **Risks:**
  - fragment cost at high render scales. `PrimFs` runs for overdraw too, under
    painter's order;
  - the derivative normal at silhouettes;
  - light on the first-person arm.
- **Mechanism measured by:**
  - `light_probe.c` extended with a light: the term against a C reference, and 0
    lights bit-identical to today;
  - the probe's light and receiver counts;
  - frame profiler and GPU timers at 0, 1, 4 and 16 lights, at render scale 1
    and 5.
- **You look at:** falloff and colour; how a light reads through the fog; a
  creature walking through a light; the look at the edge of the radius.

### Phase 2, the first slice

**What is in.** Point and spot lights, authored per area, drawn by the game's own
lighting chain:

- **Runtime `0071`**: `Gpu/RemasterUniforms.cs` holds up to 16 lights already in
  the GTE's view space, with a generation; `GlCore` uploads the list when the
  generation moves and flushes a batch built under the previous one. `PrimFs`
  gains `authored()`: the fragment's view position rebuilt from its recovered
  depth, H and the centre exactly as `NormalFs` does, its normal from that
  position's screen derivatives, and per light `colour * max(N·L, 0) * (1 -
  d²/r²)² * spot`, the spot a smoothstep between the cone's two cosines. `shade8`
  adds it times the packet's RGBC to the lit colour, **before** the depth cue, so
  the game's fog darkens it, the texture is modulated by it and it saturates
  where the game's light does. Not drawn into a planar reflection's texture, whose
  view is the mirrored camera's.
- **What a light reaches**: a packet with a `0048` record and a recovered depth.
  So map tiles, clipped fans and models, including the first-person arm (its
  position is rebuilt in view space like everything else, so the design's worry
  about view-space receivers does not arise). The HUD and anything unrecorded keep
  their vertex colour. Per-pixel lighting and Fast geometry must be on.
- **The record carries RGBC now.** A tile's, a clipped fan's and a flat model
  face's record had `0` in the low bytes, which only a directional record read;
  they carry the light colour word at `0x8006E604` that `NormalColorCol` lit them
  with. And with lights applied, a face drawn with no fog at all keeps its record
  (`PolyAssembler.KeepUnfogged`), where before it was dropped as interpolating
  the same; without a record it could not be lit.
- `patches/remaster/Lights.cs`: the area's `lights.json` behind the fingerprint;
  before each `DrawOTag` (the camera block then holds the view the table was
  built with) every light goes to view space as `R (w - cam) + T`, is culled
  behind the eye and past twelve tiles, and the nearest 16 are published. Flicker
  is value noise on a tick count taken with `FramePacing.FirstWalkOfTick`.
- The editor gains a Lights section: *Add at eye*, *Place on the picture* (the
  next click adds one 192 units short of the nearest surface the last frame drew
  there, from `Faces`' triangles), a list, and the inspector: type, position,
  colour, intensity, radius, direction and cones, flicker, on, move to eye,
  delete. Each light is drawn over the picture; a click on its dot selects it and
  a drag moves it. A held slider or drag is one undo entry.
- Shell: `light list | add NAME [here | pick GX GY | X Y Z] | remove | select |
  set NAME FIELD V...`; `list` gives each light's projected pixel and depth, and
  the counters. `KF2_REMASTER_LIGHTS=0` leaves the pack's lights out.

**Measured** (`KF2_AUTOSTART=2`, area 1 in `fdat05`, a scratch pack, 144 fps,
render scale 5, 16:9):
- **The term is the formula.** `scripts/light_probe.c` runs the real `PrimFs`
  with three lights (two points and a spot) over a wall at a known depth and
  compares every pixel with the same formula in C: worst difference **0** over
  all nine strips. With no light uploaded, and with lights uploaded but no depth,
  the strips are 0 from 0048's formula as before.
- **Off is the picture it was.** At a pinned view, `set remaster off` gave the
  hash of the frame before any light was added (`210d55698c875fb8`), and so did
  switching the light off with `light set ... enabled off`; undo went back to the
  earlier lit hash.
- **A pick places the light where it was clicked**: `light add warm pick 160 170`
  projected back to game pixel `160.1,170.1` at depth 1447. The brightest changed
  pixel was at `170,172`, and it followed the light through four more cameras
  through `view` -- yaw ±100, pitch 150, and the eye moved 800 units -- at 9-19
  pixels from the light's own projection, the offset the surface's slope gives.
  No pixel ever got darker.
- Radius 1024 changed 27.9% of the picture inside one rectangle round the light;
  4096 changed 97.9%.
- Flicker ran at 20 steps a second with the world, and three snaps with the
  editor open were identical.
- A restart read the saved `lights.json` and sent both lights. Under
  `KF2_POLYASM=verify KF2_TILEWALK=verify` with them applied: 2,517 reports, all 0
  RAM, register and GTE mismatches.
- **Cost**, 16 lights against none at render scale 5 (2140×1200): the frame's GPU
  batches 0.51 → 0.69 ms by the frame capture's timers (0.97 in a noisier first
  capture), CPU work 0.96 → 0.99 ms a frame, 144.0 fps drawn at 20.0 ticks/s
  either way, `[present] wide 288`. Off, one test a frame and one `int` compare
  per triangle.

**Not judged.** Nothing about the look: falloff, colour, how a light reads in the
fog, the edge of the radius, a creature walking through one. Nor the editor's
Lights section, the gizmos and the drag. Render scale 1 was not measured, and
neither was a light on the arm.

### Phase 3: the material system proper

- **Ships:**
  - [x] the `0067` amendment: ids widened to 256 (the `RGBA16F` alpha holds them
    exactly); the table stayed `SurfaceMaterial`'s, as a texture, not in
    `RemasterUniforms`;
  - [~] materials keyed by tile mesh (Phase 1's faces), model (in) and texture
    index hash (not started: it waits on Phase 4's key census);
  - [x] emissive, which adds to the lit term the way a light does, and roughness,
    which only SSR reads, as a blur of its hit;
  - [x] metalness, specular, occlusion, a light of its own, pulse and unfogged glow
    (the second slice).
- **Mechanism measured by:** the `ByMaterial` census for every source; the
  reflection census identical with no pack; `shader_probe.c` for emissive
  (`light_probe.c` in the event, since the term is `0071`'s).
- **You look at:** emissive surfaces in the dark areas; how roughness looks on
  SSR.

### Phase 3, the first slice

**What is in.** Two material properties, a key, and the table they need:

- **Runtime, `0067` amended**: `SurfaceMaterial.Count` 256, `BlendedFlag` 256,
  `Roughness`, `Emissive`, `Generation` and `Changed()`. `GlCore` uploads the table
  as a 256×2 RGBA32F texture when the generation moves: row 0 reflectivity, F0 and
  roughness for `SsrFs`, row 1 the emissive colour for `PrimFs`.
- **Roughness** is a blur of the reflection, not a jittered ray: nine taps over the
  footprint of the cone the reflected ray stands for, `roughness × distance`
  across at the hit's depth, skipping taps under the HUD or off the picture. A
  planar lookup takes its distance from the planar texture's depth. 0 is the single
  read it was.
- **Emissive, `0071` amended**: the packet's material rides in the light buffer
  (attribute 11), and `PrimFs` adds its row-1 colour to the authored term, before
  the depth cue and times the packet's RGBC, so a glow is fogged and textured like
  the game's own light and strength 1 shows the texture at full. It needs no depth,
  so it is drawn into a planar reflection too. Only a packet with a `0048` record
  glows, which needs per-pixel lighting and Fast geometry.
- **The first thing that did not work**: a glowing model changed no pixel. A face
  with no fog at all keeps no light record (it interpolates the same per pixel), and
  near the eye that is every face; only authored lights kept them
  (`PolyAssembler.KeepUnfogged`). It is now kept for either reason, each owned by
  its feature (`KeepForLights`, `KeepForGlow`), and only while the area has a
  glowing material applied.
- **Models**: `ModelWalk`'s submitter asks `Surfaces.EnterModel(kind, model)` before
  the assembler call and `SealDepth` writes the id as it does for a tile.
  `surfaces.json` gains `models`, keyed by kind and model id. `Faces` records a
  model's triangles with both, so a click on a model selects it; the editor tints
  its triangles and gives it the material combo. Nothing is keyed by instance.
- The editor's library gains *Roughness*, *Emissive* (a colour) and *Glow* (0-4);
  a colour held is one undo entry. Shell: `select model:A:KIND:ID`, `select pick`
  answering a model, `set model:... material NAME`, `set material:NAME
  roughness|emissiveStrength V` and `emissive R G B`; `pack list` shows models and
  the new fields; `remaster` gives `byMaterial` as only the ids drawn, the table's
  uploads and whether anything glows.

**Measured** (`KF2_AUTOSTART=2`, area 1 in `fdat05`, `KF2_SSR=1`, a scratch pack,
144 fps, render scale 5, 16:9, the view pinned with the editor open):
- **The formula.** `light_probe.c` adds three passes: glow on with material 0,
  glow on with a glowing material and no depth, glow off with the same material.
  Worst difference from the formula 0 in all six passes; the first and third new
  ones are the program as `0071` left it, to the bit.
- **Off is the picture it was.** With nothing authored, remaster on and off gave
  `210d55698c875fb8`, the hash Phase 2 recorded at this view before this change, so
  moving the table into a texture moved no pixel.
- **A model glows, and only it.** `select pick 180 100` chose
  `model:1:object:486` (63 triangles). At glow 1.5 the picture changed in 11.1% of
  its pixels, by up to 153 levels, all inside render pixels 930-1378 by 0-912, the
  model's rectangle; at 0.5, by up to 94. At glow 0 -- the records kept, the term
  zero -- and with the remaster off, the hash was the baseline's.
- A face of a floor half in area 0 at glow 1.5 changed 3.2% of the picture, all in
  the bottom rows where it lies.
- **Roughness moves only the reflection.** A mirror (reflectivity 1, F0 0.5) on a
  floor face; roughness 0.3 and 1 changed 3.6-3.9% of the picture, inside the
  floor's rectangle, by up to 15-24 levels. Back at 0 the hash was the mirror's own
  again.
- **The saved pack reproduces it.** After a restart, the same view's hash was the
  one before (`22f23314bc315e90`).
- **Verify.** `KF2_POLYASM=verify KF2_TILEWALK=verify` in area 0 with the glowing
  face applied and sealed: 899 reports over the nine routines, all 0 RAM, register
  and GTE mismatches. `KF2_MODELWALK=verify` into area 1 with the glowing model: 72
  reports, one of them 1 RAM mismatch in `func_800331B4`, in area 0 before the save
  loaded (a word at `0x80073DF4`), which is the ambient-sound key-on the walk's
  verify is documented to show ("A verify pass replays, it does not re-run" in
  `PATCHES_AND_MODS.md`) -- **Inferred**, since the probe that counts key-ons was
  not on. The picture under that verify was the normal run's hash. All three
  verifiers together run at 0.2 fps in area 0, too slow to reach a save.
- 144.0 fps drawn at 19.9-20.0 ticks/s in area 1 with the pack applied, and in
  area 0 with the glow on and with the remaster off; `[present] wide 288, plain 0,
  vram fallback 0`. One view in area 0 facing the sea, editor open, read 114 fps;
  it was not compared with the build before this and is not explained.
- **Cost:** nothing glowing, one bool a batch. Glowing, one texel fetch per
  fragment of a recorded packet, and the records of unfogged faces kept, as authored
  lights already do. Roughness 0 costs nothing; above it, eight more reads per
  reflective pixel.

**Not judged.** Most of the look: how a glow reads through the fog, roughness on a
floor mirror and on water, the editor's new controls and the model tint. One glow
was looked at; see below. Under `KF2_MODELWALK=verify` a click cannot pick a model (the last
submit recorded is the recompiled pass's, which names nothing).

**Looked at: glow reads as lit, not glowing.** Judged from play (area 0, a wall
panel at glow 4.0, roughness 1): "it kinda looks like it's glowing, but not quite".
Two causes in the design, neither of them the missing bloom:
- **It multiplies the texture.** The term is added to the lit colour *before*
  the texture is modulated, so glow 4 is "the texture at twice full, clipped": the
  texture's dark blotches stay dark and the panel reads as overexposed stone. Right
  for a lit window; wrong for a light source.
- **Nothing around it is lit.** The floor in front of the panel is as dark as
  anywhere else, and that is the strongest glow cue there is.
- Roughness did nothing in that shot, correctly: it only blurs a reflection, and
  the panel had reflectivity 0.

**Next, for glow** (agreed with the user; 2 and 3 are built, see "The glow is a
light source" below):
1. **Try by hand first**: an authored point light just in front of the panel, the
   glow's colour, radius 1500-3000. The user was to say whether that is much
   closer; if so, build 2 and 3 together.
2. **A glowing material gives off a light.** `Lights` builds the frame's list from
   `Pack.Lights(area)` (`patches/remaster/Lights.cs`, `Resolve` and the publish
   before `DrawOTag`); add derived lights for the area's glowing faces and models.
   A tile face's world position comes from its half (`Identity`: tile X, Z at 2048
   units, the floor at `-(h) << 7`) and the mesh's vertices (`Faces.Mesh`); a
   model's from `ModelWalk.Scene`. The cap is 16 lights, nearest first, so derived
   ones compete with authored ones; decide which wins. Keep it off with no glowing
   material, so off stays bit-identical.
3. **An additive glow mode**: add the emissive colour *after* the texture is
   modulated, so dark texels light too. The term is in `PrimFs`
   (`GlShaders.cs`: `extra += texelFetch(uMatTable, ivec2(int(vMat), 1), 0).rgb`
   in `main`, applied in `shade8`); row 1's alpha of the material table is free for
   a mode flag (`GlCore.BindMaterials`, `SurfaceMaterial.Emissive`). Keep the
   current mode as the other choice, extend `scripts/light_probe.c` for the new
   one, and amend `0071` (a correction to its own term, not a new mechanism).
4. Bloom is a pass on the finished picture, a separate feature, later.

**Next, otherwise.** The texture key needs Phase 4's census before anything is keyed
by it, so this phase's third key waits for that. Instances (one door, not every
door) need the slot measurement "Identity" asks for. Roughness on a mirror floor and
on water has still not been looked at.

### The glow is a light source

Items 2 and 3 above, built together.

- **An additive glow, the new default** (`0071`, amended again). Row 1's alpha of
  the material table says how an id glows (`SurfaceMaterial.EmissiveAdditive`):
  additive adds RGBC times the glow *after* the texture is modulated, so a dark
  texel lights as much as a bright one; the old mode (`"glowMode": "lit"`) is kept.
  The additive term is fogged on the packet's own depth-cue curve, like everything
  else the game draws: an unfogged glow would pop out of the black at the draw
  window's edge, where the tiles themselves are cut. `PrimFs` splits the curve out of
  `shade8` as `cueWeight()` and adds the fogged glow, `gGlow8`, to the modulated colour
  on every output path (flat, texture, replacement texture, replacement CLUT). Zero
  on every packet without an additive glow, so the other programs are unchanged.
- **A glowing material gives off a light** (`Lights`). When the area applies, every
  tile half with a glowing face gets one point light per material: the glowing
  faces' area-weighted centroid, 192 units out along their summed normal. A face's
  world corners are the half's placement (the tile centre, the floor at `-(h << 7)`)
  plus its mesh vertex (`table + 0xC + header[+0]`, 8 bytes a vertex, indexed by byte
  offset), turned by the record's quarter turn as `func_80014B88` turns the matrix:
  1 is `(z, y, -x)`, 2 `(-x, y, -z)`, 3 `(-z, y, x)`. Its normal is the one the game
  lights it with (`header[+8]`, indexed from the face), turned the same way. A
  glowing model gets a light 384 units above its origin, per draw, from
  `ModelWalk.Scene`. The light's colour is the glow's, times *Glow light*
  (`glowLight`, 0.5 by default); its radius is *Glow reach* (`glowRadius`, 2048;
  0 gives no light).
- **Authored lights win the 16 slots.** Every derived light ranks after every
  authored one, then nearest first. They were placed by hand; a derived light
  comes from any glowing face, however many there are.
- Editor: *Light source* (the mode), *Glow light*, *Glow reach* under each material.
  Shell: `set material:NAME glowMode additive|lit`, `glowLight V`, `glowRadius V`;
  `light list` gives each derived light (`glow`) with its normal and projection,
  and `modelGlow`.

**Measured** (`KF2_AUTOSTART=2`, area 1 in `fdat05`, a scratch pack, 144 fps,
render scale 5, 16:9, reflections on, the view pinned with the editor open: camera
`73709,-16448,74986`, pitch 26, yaw 2639, the same view as Phase 2's hash):
- **The formula.** `light_probe.c` gains four passes on a strip textured with a
  known 15-bit texel: no glow, the lit glow, the additive glow, and the additive
  glow untextured. Worst difference from the formula **0** in all ten passes; the
  first six read as before, to the line.
- **Off is the picture it was**: `210d55698c875fb8` with nothing authored, remaster
  on or off, and again after every glow was set back to 0. A zero-glow material that
  keeps its default reflectivity 0.3 is not the baseline with reflections on, which
  is its reflection and not the glow: at reflectivity 0 it is.
- **A wall panel** (`tile:1:37:36:upper`, face 2, colour 1,0.6,0.25, glow 1.5): the
  panel's darkest tenth of pixels had a mean luminance of 54 unglowing, **98 lit**
  and **179 additive**; the median 73, 135, 196. Only the panel's own rectangle
  changed in either mode (12.1% of the picture).
- **Its light** sits at `77376,-17248,74885` with the normal `(-1,0,0)`, towards
  the camera at X 73709, so the lit side is the side the camera can see (it would be
  backface-culled from the other). It projects inside the panel, at depth 2914. At
  glow light 0.5 and reach 2048 it changed 336,078 more pixels outside the panel, by
  up to 29 levels, and **darkened none**. A second face, a wall facing +Z, got a
  normal of `(0,0,1)` with the camera on that side too.
- A glowing model (`model:1:object:486`) got its light (`modelGlow 1`, three sent).
- The saved pack reproduced the same hash after a restart (`d8c08a2ca30faedc`).
- 144.0 fps drawn at 20.0 ticks/s with the panel's glow and light, editor closed;
  `[present] wide 288, plain 0, vram fallback 0`.
- **Cost**: a half is looked at only if something is authored on it, once per apply;
  a model's light is one dictionary lookup per model drawn, only while a model
  material gives light.

**Changed for an existing pack.** A material without `glowMode` is now additive,
so a pack saved in Phase 3 draws its glows differently, and gives off light unless
its reach is set to 0. The Phase 3 hashes above are of the old mode.

**Not judged.** The whole look: whether the additive glow and the spill now read as
a light source, the defaults (glow light 0.5, reach 2048), a model's light at 384
above its origin (a guess at the middle of a figure; a view-space model such as the
arm would get one at a meaningless position), several adjacent halves each giving
a light (they add), and fog on the glow. Bloom is still item 4.

**Looked at: glow.** "It looks pretty good." The same judgement asked for a light
that leaves the texture alone, which the second slice below adds.

**Looked at: roughness.** At 1.0 the reflection on a mirror floor turned into a
fine woven crosshatch rather than a smear; see the second slice.

### Phase 3, the second slice

**What is in.** Everything a material could still say without a texture key:

- **Roughness, without the weave** (`0067`, amended). The blur now reads a half-size
  mip chain of the picture (and of the planar texture) at the level whose texel
  spans the blur, instead of eight sparse taps turned per pixel. Roughness is
  squared before use, so the slider's lower half is the useful range.
- **Metalness** (`0067`): the reflection takes the surface's hue at full value, so a
  dark bronze tints what it reflects without darkening it.
- **Specular** (`0071`, amended): the highlight an authored light, or a glow's own
  light, leaves. Normalised Blinn-Phong, its size from roughness, added past the
  texture, fogged, and tinted by the texel on a metal.
- **Occlusion** (`0067`): how much the occlusion pass darkens a material, applied at
  the present from the surface buffer's id. A glowing material defaults to 0.
- **Light without glow**: `"light"` is the material's own light intensity, in its
  emissive colour, whether or not the surface glows; without it, a material reads as
  it did (`glowLight`, 0.5, times the glow). **A material's light does not light that
  material** (a point's outer cosine carries the id, `0071`), so a lamp with no glow
  keeps its texture exactly and a glowing panel is no longer lit by itself too.
- **Glow ignores fog** (`"glowFog": false`), additive glows only.
- **Pulse**: `pulseAmount`, `pulseHz`, `pulseStyle` breathe or flicker, on the world
  tick (`Lights.Ticks`, now counted whether or not a light is sent), so it holds while
  the world is paused. The glow and its light pulse together.
- Editor: *Metalness*, *Specular*, *Occlusion*, *Fogged*, *Light*, *Light reach*,
  *Pulse*, *Pulse rate*, *Flicker*. Shell: `set material:NAME metalness|specular|
  occlusion|light|pulseAmount|pulseHz V`, `glowFog on|off`, `pulseStyle breathe|flicker`.

**Measured** (area 1, the pinned view, reflections on, a scratch pack):
- **The weave was the roughness blur, not the reflections.** A mirror floor
  (`tile:1:37:36:upper` face 3) at roughness 0, 0.5 and 1, on the previous build and
  this one, each with the pass at 2x and at full resolution; the autocorrelation of
  the reflection's fine detail:

  | build, pass | roughness 0 | 0.5 | 1 |
  |---|---|---|---|
  | previous, 2x | no repeat | **every 8 px, +0.50** | **8 px, +0.39** |
  | previous, full | no repeat | **every 4 px, +0.72** | **4 px, +0.68** |
  | this, 2x or full | no repeat | no repeat | no repeat |

  The repeat was the 4x4 pattern at the pass's resolution (render scale 4 there),
  so the resolution set its size and not its existence. The dark band under the ledge
  in the report's shots was not checked; it is in area 0.
- **The formula.** `light_probe.c`: 14 passes, 126 cases, every one 0 from the
  formula; the first ten read as before. The skip pass lights 452 pixels of 512.
- **Off is the picture it was**: `210d55698c875fb8` with nothing authored, and again
  after each new term was set back to 0.
- **Light only** (colour 1,0.7,0.4, light 1.5, reach 3000, no glow, on the wall
  panel): all 309,703 of the panel's pixels unchanged; 601,350 around it lit; none
  darker.
- **Specular 1** on the floor under a light placed to mirror into it: at roughness
  0.5, 66,444 pixels brighter by up to 49; at 0.2, 6,245 by up to 213; none darker.
  On a metal the same pixels take the texel's colour.
- **Metalness 1** on a mirror floor changed its reflection by up to 24 levels.
- **Occlusion 0** on a wall: 210,402 pixels brighter by up to 10, none darker.
- **Pulse** 1 at 2 Hz: three snaps 0.3 s apart with the world running, three hashes;
  with the editor open, the same hash twice.
- 144.0 fps drawn at 20.0 ticks/s with all of it at once (a pulsing glowing panel
  with its light, a rough metal mirror floor with a highlight, a wall at occlusion 0
  and an authored light), editor closed; `[present] wide 288`.

**Changed for an existing pack.** A glowing material is exempt from occlusion unless
it says otherwise, and its light no longer lights itself, so a glowing panel reads a
little dimmer than in the first slice. Roughness is squared, so a saved value blurs
less than it did.

**Not judged.** All of it: whether roughness 0.2-0.5 reads as polished or wet stone
and whether the blur grows with distance as it should, highlights on the floor and
on models, metal on a mirror, a pulse's rate and a flicker's feel, a lamp with no
glow, an unfogged glow at the edge of the draw window. The blur reads the whole
picture, HUD included, at high roughness near it.

**Looked at: metalness is hard to see.** "I'm struggling to see metalness
whatsoever." Three reasons in the design: it only tints the reflection and the
highlight, so with reflectivity 0 (or reflections off) and no highlight it does
nothing; the tint is the surface's hue, and this game's stone is nearly grey
(measured: 24 levels at most); and it leaves out what makes a metal read as one --
a strong reflection looking straight at it (F0 is not raised) and a darker base
colour. **Next** (proposed, not started): metalness pulls F0 up to the reflectivity,
darkens the surface's own colour, and saturates the tint a little; whether it also
brings some reflectivity of its own when that is 0 is the user's call.

### Phase 4: textures

- **Ships:**
  - upstream replacement packs brought into the port's path, first measured:
    - does a replaced texture keep the `0041` kernel, the `0060` rectangle clamp
      and the mip atlas;
    - do the fluid slots scroll a replaced water texture through `0053`;
    - fixes by amendment to those patches;
  - normal and roughness maps, **only paired with a replacement texture** (`0074`),
    read in `PrimFs` for the light term and passed to the surface buffer for SSR;
  - a texture-key census that tells a pack author which textures of an area the
    pack covers.
- **Risks:**
  - the UV-rectangle problem in the texture key (see "Identity");
  - the atlas's 2048×2048 budget with high-resolution replacements.
- **You look at:** replaced textures under filtering, at a distance, and in
  motion; normal maps under an authored light.

### Phase 5: atmosphere

- **Ships:**
  - light-record overrides after stage 1's copy;
  - fog colour and curve (`0073`);
  - a sky fill at the far plane that respects `Overlay`, so the HUD is never
    painted over;
  - `atmosphere.json`.
- **Depends on:** the read census confirming that only rendering reads the light
  records.
- **Mechanism measured by:** `KF2_PERPIXEL_PROBE=2`, the formula against the GTE's
  colour, still exact with an override on; the SSR fog-curve readback moving
  with the new curve; `snap` for the no-override case.
- **You look at:** the whole area's mood; the sky against the void past the draw
  distance.

### Phase 6: level edits

- **Ships:**
  - `level.json` with tile byte edits, behind the fingerprint gate;
  - the rewrite census, so a door's tiles are flagged;
  - the gameplay-changing label.
- **Depends on:** measuring whether the tile block reaches a save.
- **Mechanism measured by:**
  - `goto` onto an edited height, with `state` reading a floor gap of 0;
  - `press` into an edited wall, with the position not crossing it;
  - the `KF2_MAP_PROBE=1` dump showing the edit;
  - a save made after the edit loading with the pack off and giving the
    original tile.
- **You look at:** whether edited heights meet their neighbours; lighting on the
  edited tiles, which take their light record from `+4`.

### Phase 7: the editor camera, and sharing

- **Ships:**
  - the free camera: `Stage13.ViewOverride`, with the grid following it;
  - export of the working pack as a zip;
  - a compatibility report per pack: fingerprints matched, keys resolved and
    keys missing, per area.
- **Risk:** the arm and the object walk read the player's position rather than the
  camera. If that shows from far away, the free camera is limited to near the
  player until those two are given the eye. The grid is not a risk any more: it
  follows the eye, measured.
- **You look at:** flying the camera through an area with nothing missing.

### Phase 8: later

- shadows by marching the tile grid;
- port-drawn props;
- opt-in object and creature placement;
- a GPU id buffer (`0075`), if picking is ever too slow.

### Dependencies, in one list

- Phase 1 comes before everything.
- Phase 2 needs 1.
- Phase 3 needs 1; it helps 2, since emissive shares the light term.
- Phase 4 needs 3, because maps attach to materials.
- Phase 5 needs 2's uniform block.
- Phase 6 needs 1 and the save measurement.
- Phase 7 needs 1.
- Phase 8 needs 2, 6 and 7.

### Risks, in one list

1. **The texture key is unstable across UV rectangles.** It is measured in
   Phase 4 and normalised through `GteTexRect` or the upload rectangle.
2. **Instance slots may not be stable.** They are measured before any instance
   key is used, and there is a fallback key.
3. **The arm and the object walk from a free eye** read the player's position;
   `goto` is the fallback. (The cull grid from a free eye was this risk, and is
   measured correct.)
4. **Shader cost at a high render scale**, since the port is CPU-bound and frame
   rate hides it. GPU timers per feature are the answer.
5. **Save contamination** from placement edits. Opt-in, labelled, and last.
6. **An upstream merge that touches `Assets/`.** The remaster depends on
   upstream's pack code, so the next merge (see `RECOMPONE_FORK.md`) has to
   treat `Assets/` as something the port relies on.

## Open decisions

Each has a recommendation. None of them blocks writing Phase 1 except the first
two.

1. **The first target area.** *Recommend `fdat02`*: a fixed spawn, water, and
   SSR and planar numbers already taken there.
2. **Material or light for the first slice.** *Recommend a material*: it needs no
   runtime patch and exercises every layer.
3. **In-game or external editor.** *Recommend in-game*, with hand-editable,
   watched JSON as the external path.
4. **Where packs live.** *Recommend upstream's `packs/`*: a remaster pack is an
   asset pack plus a `remaster/` directory, so replacement textures and the
   remaster ship as one thing.
5. **Forward or deferred authored lights.** *Recommend forward, inside `shade8`*,
   and deferred only for terms that apply to the finished picture.
6. **Whether object and creature edits may reach a save.** *Recommend no by
   default*: opt-in per pack, labelled, and last in the roadmap.
7. **Shadows.** *Recommend none at first, then the tile-grid march*; no per-frame
   shadow maps on a CPU-bound port.
8. **Normal maps only with replacement textures.** *Recommend yes.* A normal map
   drawn over a 64×64 four-bit texture fights it.
9. **Whether a remaster pack may change collision.** *Recommend tile collision
   only, behind a gameplay-changing label.* It keeps the rule against gameplay
   changes a player did not ask for.
10. **The material id budget.** *Recommend 256*: exact in the surface buffer's
    alpha, and allocated by name at load time.
11. **Where the remaster's code lives.** *Recommend `patches/`, not `mods/`*: the
    remaster is something the port offers, and a mod is a package that can be
    absent (see "What belongs in a mod" in `PATCHES_AND_MODS.md`). The *packs*
    are the optional part.
