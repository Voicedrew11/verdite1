# Environment variables

Every `KF2_*` switch the port reads, in one list. Most are wired up in
`Program.cs` or read by the patch they belong to (`patches/*.cs`); the document
named for each feature in `NOTES.md` explains what the comparison is for. A
variable marked *no longer a setting* is the only way left to reach that
behaviour — its control came out of the settings window and its saved key is not
read.

Prefer the mods under `mods/` (enable them in the game's Mods panel) to
`KF2_LOG=sdk`, which is gigabytes a minute. `KF2_LOG=bios` is worse during play:
the game polls `PAD_dr` hundreds of thousands of times a second.

```bash
KF2_LOG=bios,cd,gpu,dma,sdk,spu,mdec  # or KF2_LOG=all; wired up in Program.cs
KF2_CDTRACE=1                          # stack trace on first CD register access (patch 0002)
KF2_AUTOPAD=8:Start:400,20:Circle:200  # scripted pad input: seconds:button:holdMs
KF2_FPS=120                            # King's Field: 60 (default), 20..360, or off (the game's own 20); see "Frame pacing" in KF1.md
KF2_FPS_PROBE=2                        # King's Field: also diff guest RAM across 200 redraws, for state the renderer steps
KF2_STAGE_PROBE=1                      # King's Field: the main loop's stages per second: calls, ms, projections, DrawOTag calls
KF2_STAGE_PROBE=flip:2                 # also turn the renderer's camera halfword at 0x80065098+N by half a turn for its call
KF2_STAGE_PROBE_FUNCS=8001E83C+8001F218  # measure these routines instead of the stages (up to 15)
KF2_RAM_WATCH=8009522C,800597D4        # name the recompiled routine behind the first store to each word (0084)
KF2_AUTOPAD_FROM=game                  # King's Field: start KF2_AUTOPAD's clock at GAME.EXE's load (OPEN.EXE's by default)
KF2_TICKRATE=30                        # ticks a second the world runs at (20, and no longer a setting)
KF2_FPS_GATE=80037C0C+8002A550+80040348+80046A60+8004910C+80033FBC+8002DC78  # what is ticked
KF2_FPS_LOGIC=full                     # no gating; scale the movement deltas instead
KF2_FPS_PROBE=1                        # a line a second: fps drawn, presents, ticks taken, what each smoother is doing; a `no boundary` line while the watchdog paces
KF2_PROFILE=1                          # the frame profiler from boot, a console summary every 5 s; =panel also opens it (Shift+P toggles)
KF2_PROFILE_OUT=profile.csv            # every frame's sections as CSV; scripts/profile_report.py reads it
KF2_PROFILE_SPIKE=12                   # a console line for each frame over 12 ms of work, with its top sections
KF2_PROFILE_FUNCS=stages               # time the thirteen main-loop stages, or name functions: game:80040348+800342D8
KF2_PREJIT=0                           # compile each method on its first call again (it is warmed on a background thread by default)
KF2_PREJIT_PROBE=1                     # a line per overlay as it is warmed, and what each cost
KF2_FRAMEVIEW=panel                    # open the frame viewer at boot (Shift+F toggles it)
KF2_FRAMEVIEW_CAPTURE=20,40            # capture stage 13 at these seconds after boot; a summary on the console
KF2_FRAMEVIEW_OUT=dir                  # also write each capture's commands and calls as CSV there
KF2_MENUPACING=0                       # menu cursor repeat and blink back on the frame clock (on by default)
KF2_MENUPACING_PROBE=1                 # what each repeat cost, and the blink's step rate
KF2_MESSAGETEXT=1                      # sign and dialogue text drawn as text on an opaque box (experimental; off -- judged and rejected)
KF2_MESSAGETEXT_PROBE=1                # each message's (file, entry), its decoded lines or why it was left alone, and what the panel drew
KF2_MESSAGETEXT_TEST=3:0,6:360         # open these messages from the main loop 10 s in; KF2_AUTOPAD dismisses them
KF2_MENUWORLD=0                        # a menu, shop, sign or dialogue pastes the frozen 320-wide frame again (the world is drawn live by default)
KF2_MENUWORLD_PROBE=1                  # world passes a second, primitive bytes used, overflows, sessions refused; a line per message fade
KF2_LOOPPACING=0                       # loops that render their own frames back on the render rate (on by default)
KF2_LOOPPACING=pace                    # hold such a loop but do not redraw: right speed, tick-rate picture
KF2_LOOPPACING=nocarry                 # redraw, but do not carry a view the loop pans itself
KF2_LOOPPACING_PROBE=1                 # modal frames a second, world and interface, against the main loop's
KF2_LOOPPACING_PROBE=2                 # also how far the loop's own view moves per iteration
KF2_LOADPACING=0                       # the loading screen's walking figure back on the host ceiling (paced by default)
KF2_LOADPACING_PROBE=1                 # its steps a second, and what one load cost
KF2_TINTHOLD=0                         # the recompiled stage 1: the death fade and damage flash cleared between ticks, so they strobe (held by default)
KF2_TINTHOLD=verify                    # run both stage 1s on every call and compare RAM and registers; the recompiled result stands
KF2_SPRITEANIM=0                       # billboard sprite animation back on the render rate (paced by default)
KF2_SPRITEANIM_PROBE=1                 # cel changes a second, live slots, and how many walks stepped
KF2_RATECENSUS=1                       # rank memory by whether it moves at the render rate
KF2_RATECENSUS_RANGE=80060000:801C0000 # the window to watch (this is the default)
KF2_RATECENSUS_OUT=path KF2_RATECENSUS_PERIOD=5   # where to dump, and how often
KF2_BLACKPROBE=1                       # what the picture held every drawn frame around an area load: luminance, black runs, the tint the game asked for, whether the frame's pixels were new, and whether the view was carried; camgap and camstep rows: how far the renderer's camera sat from the true position, and how far it moved per present
KF2_BLACKPROBE_OUT=dir                 # also write the display rect (~12 frames before the load and 250 ms after) as PNGs; default scratch/blackprobe; 0 skips the pictures
KF2_SMOOTH=0 KF2_SMOOTH_POS=0          # leave the view at the tick (on by default); leave position too
KF2_SMOOTH_COMPASS=0                   # leave the compass needle on the tick (carried with the view by default)
KF2_SMOOTH_GAUGES=0                    # leave the HP and MP gauges on the tick (carried with the view by default)
KF2_SMOOTH_PROBE=1                     # how far the view is being carried, per second; the compass's and the gauges' carries and new values
KF2_SMOOTH_PROBE=2                     # also every frame for 400 ms after an area load: the verdict, the pair, and what was written
KF2_CROSSPROBE=1                       # per presented frame across an area crossing: tiles, models, the view, the area, and whether the renderer built it; =2 adds the raw cell counts
KF2_SMOOTH_OBJECTS=0                   # leave enemies, doors and everything else at the tick (on by default)
KF2_SMOOTH_OBJECTS_PROBE=1             # how much is being carried, per second
KF2_SMOOTH_OBJECTS_GUARD=continuous    # strict|sticky|continuous: what counts as a placement (creatures)
KF2_SMOOTH_ANIM=0                      # leave MO pose at the tick (on by default)
KF2_SMOOTH_ANIM=time                   # lerp the clip time between the two ticks (the default mode)
KF2_SMOOTH_ANIM=timeline               # comparison: interpolate on the clip's own timeline
KF2_SMOOTH_ANIM=weight                 # comparison: the blend weight only, inside the game's segment
KF2_SMOOTH_ANIM_PROBE=1                # morph vs rigid submits, the verdict census, carries
KF2_SMOOTH_FLUID=0                     # leave scrolling textures (water, slime skins) on the tick (on by default)
KF2_SMOOTH_FLUID_PROBE=1               # live slots, dest rects, leftover V shift, whether the uniform is bound
KF2_HITGUARD=0                         # let the hit path's reaction lookup fault (it is fenced by default; docs/TODO.md #14)
KF2_CRASHDUMP=0                        # no game-state dump on an unhandled exception (it dumps by default)
KF2_HITPROBE=1                         # census what the hit check saw; =2 every call
KF2_DRAWCENSUS=1                       # which renderer routine drew how much of the frame; =2 names the models
KF2_TEXPROBE=1                         # textured vs flat prims a second, and a per-page VRAM census, into texprobe.log
KF2_WIDESCREEN=4:3 KF2_WIDESCREEN_PROBE=1  # aspect (16:9 by default), and the margin census
KF2_WIDESCREEN_PROBE=2                   # the census plus every wide primitive, once per shape
KF2_WIDESCREEN_EFFECTS=0                 # leave the death fade and damage flash 320 wide (stretched by default)
KF2_WIDESCREEN_HUD=1                     # anchor the HP/MP panel and icons to the new edges (off; no longer a setting)
KF2_WIDESCREEN_CULL=0                    # leave the game's view cone at its 4:3 shape (widened by default)
KF2_WIDESCREEN_CULL=1.5                  # pin a widening factor instead of the aspect's
KF2_WIDESCREEN_CULL_PROBE=1              # tiles lit, and what the 24x24 grid clipped
KF2_WIDESCREEN_CULL_PROBE=2              # also lit-per-ring after the occlusion flood
KF2_WIDESCREEN_CULL_PROBE=3              # also rebuild the stock grid each frame: stock tiles missing from the widened one (must be 0)
KF2_PRIMBUF=1                            # the game's primitive buffers where it put them (moved above 2 MB, 4x as large, by default; =N for N x)
KF2_PRIMBUF_PROBE=1                      # the frame's primitive budget: peak, capacity, overflows
KF2_RAMSIZE=8                            # guest RAM in MB, at least what the buffers need (4 by default, 2 with KF2_PRIMBUF=1)
KF2_RAM_PROBE=1                          # with the probe line, guest accesses above 2 MB per 64 KiB page, in the buffers and elsewhere
KF2_POLYASM=0                            # the recompiled polygon assembler instead of the C# one (on by default)
KF2_POLYASM=verify                       # run both on every call and compare RAM, registers and the GTE; the recompiled result stands
KF2_POLYASM_REJECT=0                     # send every oversized polygon to the view-space clipper, including those it clips to nothing
KF2_POLYASM_REJECT=replay                # a rejection also writes the clipper's scratch records and lists, which nothing reads
KF2_POLYASM_UNCLIPPED=0                  # the recompiled func_8002FECC (the far map tiles' assembler) instead of the C# one
KF2_POLYASM_TRANSFORM=0                  # the recompiled func_8002E650, func_8002E7CC and func_8002E910 (the vertex transforms, the last the HUD's) instead of the C# ones
KF2_POLYASM_LIT=0                        # the recompiled func_8002F214 and func_8002EAEC (the models' lit assembler) instead of the C# one
KF2_POLYASM_CLIPPER=0                    # the recompiled Clip4FTP and Clip3FTP (the view-space clipper) instead of the C# ones
KF2_POLYASM_FACING=0                     # cull a clipped polygon or a quad on its first three whole-pixel corners, as the game does (the whole polygon, at its fractional corners, by default)
KF2_TILEWALK=0                           # the recompiled map-tile walk instead of the C# one (on by default)
KF2_TILEWALK=verify                      # run both on every call and compare RAM, registers and the GTE; the recompiled result stands
KF2_TILEWALK_CELL=0                      # the recompiled func_80031B1C (one cell's two stacked halves) instead of the C# one
KF2_TILEWALK_TILE=0                      # the recompiled func_80031950 (one half, set up and assembled) instead of the C# one
KF2_TILEWALK_PROBE=1                     # cells on the map, cells with a flag, halves drawn, and which assembler drew them; polygons kept by the whole-polygon cull
KF2_MODELWALK=0                          # the recompiled object and creature walk instead of the C# one (on by default)
KF2_MODELWALK=verify                     # run both on every call and compare RAM, registers and the GTE; the recompiled result stands
KF2_MODELWALK_WALK=0                     # the recompiled func_800331B4 (the four table walks) instead of the C# one
KF2_MODELWALK_SUBMIT=0                   # the recompiled func_80032588 (one model, set up and assembled) instead of the C# one
KF2_MODELWALK_PROBE=1                    # live slots per table, models submitted, by table and by assembler, and ambient key-ons
KF2_STAGE13=0                            # the recompiled renderer func_800342D8 instead of the C# one (on by default; Stage13.ViewOverride needs it)
KF2_STAGE13=verify                       # the recompiled renderer draws; the C# one is replayed against a record of its calls and compared
KF2_STAGE13_NEEDLE=0                     # step the compass needle's spring every frame drawn, as the routine does (on the world tick by default)
KF2_STAGE13_PROBE=1                      # frames drawn and needle steps a second
KF2_CAMERABLOCK=0                        # the recompiled camera block func_8002E22C instead of the C# one (on by default)
KF2_CAMERABLOCK=verify                   # run both on every call and compare RAM, registers and the GTE; the recompiled result stands
KF2_GTE_FAST=0                           # the GTE's general path for the lighting, depth-cue, RotTrans and RotTransPers ops (0047)
KF2_GTE_LIGHTCACHE=0                     # light every normal afresh instead of remembering its matrix products
KF2_VIEWCLIP=0 KF2_VIEWCLIP_PROBE=1      # the game's view-space clip volume, and where it cuts
KF2_NODITHER_PROBE=1                   # where the dither bit comes from, and GPUSTAT bit 9
KF2_TRUECOLOR=0                        # 15-bit shaded output (24-bit is the default; GL backend only)
KF2_VRAMSNAP=0                         # a menu restores the frozen frame at 1x again (the scaled copy is kept by default)
KF2_VRAMSNAP_PROBE=1                   # frame restores served from that copy, against uploads that missed, and readbacks outside a display target not copied
KF2_GLDEBUG=1                          # a debug GL context, and every error or warning the driver reports on stderr (0065); =2 adds notifications and each first report's stack
KF2_VRAMCHECK=1                        # mirror every VRAM upload, copy and fill on the CPU and name the operation that changes texels it should not (slow)
KF2_VSYNC=block                        # upstream's blocking vblank timeline instead of the port's grid (caps the picture at 60)
KF2_SWAP=interval                      # with VSync on, windowed or Borderless on Windows: the driver's swap interval, swapped at the next present, not the display's own vblank (0066)
KF2_SWAP=immediate                     # with VSync on, the driver's swap interval, each frame swapped where it was composed
KF2_SWAP=vblank                        # with VSync on, the display's own vblank in Fullscreen too (it tears there)KF2_PERSPECTIVE=0                      # affine textures again (correction is on by default)
KF2_PERSPECTIVE_PROBE=1                # the GTE vertex map's hit rate
KF2_PERSPECTIVE_FALLBACK=1             # also guess by screen position on a miss (the old mechanism)
KF2_ANISO=8                            # anisotropic filtering: taps along the footprint's long axis (16 by default; 1, off)
KF2_ANISO_PROBE=1                      # the level, and whether the uniform reaches the shader; with a filter on, the atlas every 5 s
KF2_ANISO_PROBE=2                      # also read four decoded textures back out of the atlas
KF2_MIPMAPS=0                          # no mipmaps (on by default: minified textures decoded into an atlas with a mip chain each, 0060)
KF2_PERPIXEL=0                         # corner colours instead of per-pixel lighting (on by default; GL core only)
KF2_PERPIXEL_PROBE=1                   # packets recorded, polygons lit per pixel and not
KF2_PERPIXEL_PROBE=2                   # also the shader's formula at every recorded corner against the GTE's colour
KF2_EVENFOG=0                          # the game's fog and light edges (even fog and lighting is on by default; needs Fast geometry)
KF2_EVENFOG_BLEND=0                    # no fog blend: the game's hard fog edge between records
KF2_EVENLIGHT=0                        # no light blend: the game's hard colour matrix and back colour edge
KF2_SUBPIXEL=0                         # whole-pixel vertex positions (sub-pixel is on by default)
KF2_SUBPIXEL_PROBE=1                   # how far vertices actually move, in pixels; also polygons drawn back to front, faces the fractional cull changed, and the HUD's vertices placed with a fraction
KF2_SUBPIXEL_CULL=0                    # cull the C# assemblers' faces on whole pixels again, as the game does (fractional corners by default under sub-pixel)
KF2_PGXP=1                             # upstream's PGXP as the vertex source (off; the address map answers)
KF2_PGXP_TEXTURE=0                     # its share of perspective correction off
KF2_PGXP_CULLING=0                     # leave backface culling on truncated positions
KF2_PGXP_CPU=0                         # no per-instruction register tracking (and so no RAM shadow)
KF2_PGXP_MEMORY=0                      # no RAM shadow
KF2_PGXP_VERTEXCACHE=0                 # no screen-position fallback
KF2_PGXP_CACHEW=0                      # let that fallback answer positions but not depths
KF2_PGXP_TOLERANCE=2                   # how far a recovered position may sit from the packet's; -1 off
KF2_PGXP_PROBE=1                       # its coverage, and where each answer came from
KF2_ZBUFFER=0                          # painter's order (the Z-buffer is on by default; Video ▸ Enhancements)
KF2_ZBUFFER_THRESHOLD=300              # restart the depth buffer when the scene jumps forward (0, off; no longer a setting)
KF2_ZBUFFER_PROBE=1                    # how many triangles actually depth-tested
KF2_ZBUFFER_PROBE=2                    # the frame's polygon census, and a map of the depth buffer
KF2_ZBUFFER_SOURCE=packet              # depth from the C# assemblers' packet records (0050); King's Field has none yet, so the address map is the default
KF2_ZBUFFER_BIAS=1 KF2_ZBUFFER_SLOPE=0.5  # coplanar tolerance on the test: SZ units, and pixels of depth slope; 0 0 is exact (0051)
KF2_BLENDORDER=0                       # blended surfaces (water) in table order again, painted over by opaque geometry behind them that the table put later (drawn after it by default, 0079)
KF2_BLENDORDER_PROBE=1                 # a line every 2 s: packets held and passed, and opaque samples drawn behind a nearer translucent one, models and tiles
KF2_AO=0                               # ambient occlusion off (on by default; GL backend only)
KF2_AO_RADIUS=512 KF2_AO_STRENGTH=0.8  # how far it reaches, in world units, and how dark it goes
KF2_AO_QUALITY=low                     # low (1x, 8 samples), medium (2x, the default), high (the render scale)
KF2_AO_BIAS=0.08 KF2_AO_SAMPLES=16 KF2_AO_MAXDEPTH=24000
KF2_AO_NORMALS=0                       # normals from the depth buffer again, not from the frame's geometry (0058)
KF2_AO_SOLID=0                         # a blended object-table model (the secret door) writes no depth for the pass again
KF2_AO_WORLD=1                         # also occlude against the area's own tile grid, so geometry off screen counts (0059; off)
KF2_AO_WORLD_STRENGTH=0.6 KF2_AO_WORLD_RADIUS=3072   # how dark that term goes, and how far it reaches
KF2_AO_WORLD_PROBE=1                   # the transform, the grid, and whether either is missing; =2 the camera's own tile
KF2_AO_PROBE=1                         # coverage, the projection read off the GTE, passes run
KF2_AO_PROBE=2                         # also read the occlusion back: how dark, how much, and where
KF2_MURK=1 KF2_MURK_DISTANCE=2654      # murky water: a dark murk by the distance the view ray runs through water, surface to floor, 63% at the distance (off by default; independent of any reflection; GL core only; depth and colour are also sliders under Video ▸ Experimental ▸ Murky water)
KF2_WAVES=1                            # water waves: a swell moves the water's vertices, ripples push and shade its texture (off by default; GL core only for the ripples; the settings are sliders under Video ▸ Experimental ▸ Water waves, and the `waves` shell verb)
KF2_WAVES_PROBE=1                      # a line every 2 s: rects, the clock, rippled batches, water positions free/rim/shared, halves and vertices moved
KF2_SSR=1                              # screen-space reflections on water (off by default; independent of the murk, planar and retained reflections; GL core only)
KF2_SSR_STRENGTH=0.6 KF2_SSR_F0=0.12   # how much water reflects at a grazing angle, and looking straight down
KF2_SSR_DISTANCE=16384 KF2_SSR_STEPS=32 KF2_SSR_THICKNESS=256   # the march: how far (unset: to where the game's fog turns black), how many steps, how far behind a depth still hits
KF2_SSR_SKY=1                          # a miss takes the last background pixel it crossed, not under the HUD, at this weight; 0 none
KF2_SSR_FOGCURVE=2                     # the depth-cue curve a reflection's longer path is fogged on: 0 none, 1 offset, 2 knee (default), 3 half, 4 linear
KF2_SSR_RESOLUTION=2                   # the pass at this multiple of the game's pixels; 0 the render scale
KF2_SSR_PROBE=1                        # passes, water rects and triangles, 2D overlays, refusals by blend, the fog curve, a readback of what each reflective pixel found, and a material map
KF2_PLANAR=1                           # planar reflections: the world walked again from a camera mirrored in the water (off by default; GL core only)
KF2_PLANAR_TOLERANCE=48 KF2_PLANAR_BIAS=8   # how far off the plane a surface may be and take it; how far above it geometry must be to be reflected
KF2_PLANAR_RIPPLE=4                    # how far the water's own texture bends the reflection, game pixels per unit of brightness change; 0 a flat mirror
KF2_PLANAR_PROBE=1                     # the plane, the mirrored walk and its arena, captures, binned water, and the readback with its check on the mirror
KF2_RETAINED=1                         # reflections drawn from the retained scene: the map and models kept on the GPU in world space (off by default; GL core only; stands the planar walk down)
KF2_RETAINED_PLANAR=0 KF2_RETAINED_CUBE=0   # leave out its planes, or its camera cubemap (which otherwise replaces the screen march)
KF2_RETAINED_CUBESIZE=256              # a cubemap face's size in pixels
KF2_RETAINED_CULL=0                    # draw the faces a mirror or a cube face sees from behind (culled by default, as the game culls them)
KF2_RETAINED_GATE=0                    # reflect every map half, not only those the frame's own tile walk drew
KF2_RETAINED_PROBE=1                   # the mesh and its check against the GTE's own vertices, the planes, GPU time per draw, chunks culled, and (with KF2_SSR_PROBE=1) the planar-vs-cubemap agreement
KF2_REMASTER=1                         # apply the working pack (off by default; the editor's checkbox saves it); Shift+E opens the editor
KF2_REMASTER_PACK=dir                  # the working pack (packs/working)
KF2_REMASTER_PROBE=1                   # a line every 2 s: the area, its fingerprint, what applied; a diff of the tile bytes when an area's fingerprint changes
KF2_REMASTER_LIGHTS=0                  # leave the pack's authored lights out (they apply with the remaster by default; 0071)
KF2_REMASTER_ATMOS=0                   # leave the pack's light-record overrides out (they apply with the remaster by default)
KF2_REMASTER_SHADOWS=0                 # no shadows from the authored lights (on by default: a depth cubemap per light from the retained map, 0077)
KF2_REMASTER_SHADOW_MODELS=0           # only the map casts: creatures and objects stay out of the lights' cubemaps (they cast by default)
KF2_REMASTER_SHADOW_SIZE=1024          # a shadow cubemap face's size in texels (64-4096)
KF2_REMASTER_SHADOW_BIAS=6 KF2_REMASTER_SHADOW_OFFSET=1.5 KF2_REMASTER_SHADOW_SOFT=1.25   # compare bias in world units; normal offset and filter spread in texels (the `light shadows tune` verb sets them live)
KF2_LIGHTCENSUS=1                      # who reads and writes the area's light records, by stage, every 5 s (slow: every RAM access takes the slow path; run with KF2_PRIMBUF=1)
KF2_FACE_PROBE=1                       # every tile face given one of the ids 4-7 by its key; checks the subdivider's face order and a pick from the frame's triangles against the GPU's ids (needs a reflection or the murk on, and KF2_SSR_PROBE=1; overrides any authored material)
KF2_TEXCENSUS=1                        # the texture-key census: keys, art, overlapping rects, dynamic places, what a pack covers, uploads, every 5 s; the `textures` shell verb saves it (0073)
KF2_TEXKEY=face                        # key a replacement on the face's texture rectangle, not the image the game uploaded it as; =triangle each triangle's own UVs, as upstream does
KF2_ANALOG=0                             # twin-stick control off (it is on by default)
KF2_ANALOG_TURN=1.0 KF2_ANALOG_MOVE=1.0 KF2_ANALOG_DEADZONE=0.15  # its sensitivities
KF2_ANALOG_INVERTY=1 KF2_ANALOG_PROBE=1  # look-Y inversion, and the control-state report
KF2_KEYS=stock                           # RecompOne's own key bindings; the port ships WASD
KF2_MOUSE=0                              # mouse look off (it is on by default; Escape captures the pointer)
KF2_MOUSE_TURN=1.0 KF2_MOUSE_LOOK=1.0 KF2_MOUSE_INVERTY=1   # its sensitivities and look-Y
KF2_MOUSE_BUTTONS=Square,Triangle,Cross  # left, right, middle, as pad buttons
KF2_MOUSE_KEY=Escape                     # the key that captures and releases
KF2_MOUSE_LEAD=0                         # mouse look waits for the tick, as with Gameplay ▸ Instant mouse look off (on by default); =1 forces it on
KF2_MENUMOUSE=0                          # the menu pointer off (on by default)
KF2_MENUMOUSE_PROBE=1                    # the layout table, the pointer's row, and what it did
KF2_AUTORELOAD=1 KF2_AUTORELOAD_SLOT=0   # reload the last save on death
KF2_AUTORELOAD_DELAY=2.0                 # seconds of the death first (2.0; no longer a setting)
KF2_AUTOSTART=2                          # boot straight into save slot 1..3, past the title menus
KF2_AUTOSTART=new                        # the same, but stay in the New Game (fdat02, which has scrolling water)
KF2_BOOTEXE=end                          # boot straight into OPEN.EXE, GAME.EXE or END.EXE
KF2_ENDINGEXIT=0                         # leave "The End" hanging, as the original does (a button exits by default)
KF2_AGENT=1                              # [KF2-AGENT] state lines on stdout: overlay, inGame, HP/MP/area/slot
KF2_SHELL=1                              # TCP 127.0.0.1:27900 line protocol: state|nearby|load|warp|press|kill|goto|view
KF2_UISCALE=1                            # force the interface scale, and save it
KF2_ICON=orb                             # the shipped verdite mark as the window icon (the game's memory-card icon, off the disc, by default)
KF2_ICON=off                             # no window icon at all
KF2_ICON=1                               # which of the card icon's three frames (0)
KF2_ICON_INSTALL=0                       # do not write the icon into the desktop's icon theme (Linux; it is what a Wayland compositor reads)
KF2_MAP=0                                # the map off entirely (on by default); M opens it
KF2_MAP_MINIMAP=1                        # the corner minimap on (off; no longer a setting); N toggles it
KF2_MAP_MARKERS=1                        # creatures, objects, effects and sprites on (off; no longer a setting)
KF2_MAP_STYLE=blueprint                  # the port's original blueprint plan (the game's own map; no longer a setting)
KF2_MAP_SHADE=1                          # colour each tile by its height byte (off; no longer a setting)
KF2_MAP_WALLS=1                          # tint the tiles whose +4 bit 0x80 is set (off; no longer a setting)
KF2_MAP_PAUSE=0                          # leave the world running while the full map is up (it pauses; no longer a setting)
KF2_MAP_FLOOR=lower                      # pin a stacked half instead of following the player (no longer a setting)
KF2_MAP_ARROW=1                          # the player as an arrow with a heading, not a dot (no longer a setting)
KF2_MAP_PROBE=1                          # dump the 80x80 tile grid as ASCII, its occupied extent and a marker census, and open both maps
KF2_MAP_FOG=0                            # fog of war off: the whole floor plan (on by default)
KF2_MAP_FOG_LOS=0                        # its line-of-sight gate off (on; no longer a setting)
KF2_MAP_FOG_PROBE=1                      # tiles seen, tiles lit now, tiles refused, records, flushes
KF2_MAP_FOG_PROBE=2                      # also the raw 24x24 grid, the gate's verdict and its walls
KF2_SPU_INTERP=sinc                      # voice interpolation: gauss (the console's), cubic, sinc (default); also XA
KF2_REVERB=hardware                      # legacy, hardware (the console's resampling FIR), enhanced (FDN, default)
KF2_AUDIO_PROBE=1                        # voices, clamps, mixer cost, underruns, levels, reverb preset, device rate
KF2_AUDIO_DUMP=dir                       # the final mix and the reverb return as WAVs; scripts/audio_spectrum.py reads them
KF2_POSAUDIO=off                         # sound effects aimed at their source while they play: off, speakers, headphones (default)
KF2_POSAUDIO_PROBE=1                     # tags a second, key-on timing, live tags, mixer peak; the first 24 tags against the game's own L/R
```

