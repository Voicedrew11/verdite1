# The online player's model

Other players in an online room are drawn as a knight of our own, made to look like the game's
models, holding the player's real weapon and swinging it with the game's own animation
(`src/game/online_player_render.cpp`, in `patches/src/game.patch`), and walking in a gait shaped on
the game's skeletons'. Each of their updates is played back here, a few behind, so every update
of a walk and a swing is drawn, on the height they stand at.

`tools/player_model/` holds it:

| File | What it is |
| --- | --- |
| `kfmodel.py` | Reads the game's formats (MIX chunk streams, model archives, assets with morph clips, TMD, TIM) and renders a model to a picture, culling faces as the game does. |
| `body.py` | Builds parts from lofted cross-sections, boxes and panels, and writes them as a TMD. |
| `knight.py` | The knight's part table, its helmets, and the rig that poses it. |
| `arm.py` | The weapon hold: a first-person weapon model's gauntlet and weapon in the knight's hand, at rest and through an attack. |
| `walk.py` | The stance and the walk: legs and free arm standing ready, breathing and through a cycle, and how far the body drops onto the planted foot. |
| `preview.py` | The command: `survey`, `floor N`, `knight`, `helmet H`, `swing W`, `rest W`, `walk [W]`, `gltf`, `tmd OUT`, and `cpp`, which writes the knight, its helmets and its colour offsets into `port/ext/online_player_model.cpp`. Pictures go to `build/player_model/`. |
| `gltf.py` | Writes a posed model as `knight.glb`, to turn around in Blender. |

After changing the knight, run `preview.py cpp` and rebuild; the part order is
`kf::OnlinePlayerPart`'s, and the command refuses a knight whose parts are not in it.

The pictures hold the game's textures, so they stay under `build/` and are never committed; the
knight's own geometry is ours and can be.

## What the game's models are

- Human-sized models are 300 to 420 vertices, one TMD object each, mostly smooth-shaded
  triangles, some textured. Floor 2's standing knight (map-event model 0, 308 vertices, 2012
  tall) is the reference: grey metal on page 151, a few flat colours (sleeves, knee bands).
- That knight's texture is one side-on projection of a grey metal swatch over the whole body
  (U from depth, V from height), so front faces smear one texel column across: the streaked
  look. The knight is textured the same way.
- Faces are culled by screen winding (`render_face_winding > 0` is kept), a quad on its first
  triangle; in model space `cross(v1 - v0, v2 - v0)` points inward. Actor models face -z with
  +y down, so their right is -x.

## Textures it may use

Only the common pages from `COM/MIX.TIM`, loaded once at start and kept by every floor. Only
cutscene archives (`B0/MIX*`) overwrite them, and gameplay reloads the common resources after:

| Page | Palette | Holds |
| --- | --- | --- |
| 142 (8-bit at 896, 0) | row 489 | the weapon arms' metal, chainmail, wood, spell effects |
| 151 (8-bit at 448, 256) | row 493 | the NPC knights' grey metal, cream cloth, leather, olive, green |

So the knight costs no texture memory and looks the same on every floor.

## The knight

461 vertices in 13 parts (pelvis, torso, head, upper arms, forearms, thighs, shins, hands), each
its own TMD object with its vertices relative to its pivot, about 1850 tall (the player's collision
height is 1700), then the helmets. Grey metal from page 151 for the armour, page 142's darker metal
for the gauntlets, as the weapons' own gauntlet, leather belt and boots. Tabard, sleeves, knee
bands and crest are in the player's colour, to tell players apart.

- **The colour** is the player's profile's ([COOP_PLAN.md](COOP_PLAN.md), "The profile"). Those
  faces are flat-coloured primitives written in the first colour (`PLAYER` in `body.py`), and the
  model lists where each holds its R, G, B (152 of them); `online_player_tmd_in` paints a copy
  of the TMD per colour the first time one is drawn, 34 KB each, 8 at most.
- **The helmet** is a TMD object of its own after the parts (`helmets()` in `knight.py`, in
  `online_player_helmet_count`'s order), its vertices relative to the head's pivot, drawn with
  the head; the head keeps only the mail coif. A new helmet is an entry at the end of `knight.py`'s
  `HELMETS` (a profile keeps the helmet's place in the list), its name in
  `port/ext/coop_profile.cpp`, and the count raised; `preview.py cpp` refuses a count that differs.
  `preview.py knight` draws the knight with the first helmet, `preview.py helmet H` helmet H close
  up beside floor 2's knight's head, then in the game's depth order, then on the whole knight. The
  metal's projection box is a constant (`body.py`), so a new helmet moves no texture on the others.
  - **Great helm** (68 vertices): a T visor and the crest.
  - **Fluted helm** (164 vertices, after Demon's Souls'): a skull ribbed front to back, whose ribs
    make concentric arcs from the side, the steps between them dark and the comb in the player's
    colour, as the game's own knight has its fin; a brow with a middle peak and flared temples;
    two dark crescents for eyes; a visor drawn out to a beak in three tiers, the undercuts between
    them dark for the breaths, its cheeks in the gauntlets' darker metal, slanting up to the hinge
    so the piece reads apart from the skull. The game's models paint such detail as polygons and
    leave out what would be holes, so the reference's rosette and pierced windows are left out.

## The weapon

`WEPON/WEPxx.MIM` (one asset each, loaded into asset 20 for the equipped weapon) is not a
weapon alone: it is a forearm, a gauntlet and the weapon, 200 to 370 vertices, with one clip (six
keyframes for 4096 of attack phase; seven for the Colichemarde's thrust), turned into the camera's
space by the weapon table's own first-person rotation (`COM/COM.DAT` chunk 2, 44-byte records,
yaw mirrored on load); the table's translation and projection distance are tuned for the view.
All its textures are on pages 140 and 142, both common, so any player's weapon can be drawn on any
floor. Measured over the twelve:

- The pieces are separate meshes. The forearm (102 vertices) runs from the elbow, the model's
  origin, and is open at both ends: made for the one view, it shows holes from any other. The
  gauntlet (44) and the weapon are closed but for a few edges on some crossguards' fins.
- The gauntlet and weapon move as one rigid piece through the clip (within 4 units); the forearm
  bends.
- The clip's last frame, a low guard the first-person view never shows at rest (it draws no weapon
  then), is the rest shrunk to 0.74 to 0.81; every other frame is within 5% of the hit's size.
  Drawn at rest, it made the weapon grow by 40% as a swing began. The Colichemarde's clip keeps
  its size.
- At 0.5 of the hit's size the weapons are as long as they should be beside a 1850-tall knight:
  the Short Sword 718 (0.7 m), the Bastard Sword 1245, the Colichemarde 1197.
- The swings are an overhead chop: the blade back over the shoulder in the wind-up (the hand above
  the head), up at the hit, down ahead after it; the Colichemarde thrusts.

The knight holds the gauntlet and weapon in its right hand, its own hand not drawn
(`kf/platform/online_weapon_hold.h`, unit-tested):

- Drawn from the hit's vertices, halved, the model's forearm folded onto the wrist (the end of its
  long axis) so its faces have no area and are culled; the wrist on the end of the knight's own
  forearm.
- Three vertices of the gauntlet and weapon, far apart, are followed through the clip: the frame
  they make at a phase against the hit's is the piece's turn, in the body after the table's
  rotation and a half turn (the camera looks along +z, actors along -z).
- At rest (standing or walking) the upper arm is 18 degrees forward and 9 out and the forearm 68
  further, so the hand is ahead of the belt; the blade points ahead and 42 degrees up, its flat to
  the side.
- Through a swing the forearm points from the elbow toward where the clip has the wrist, and the
  upper arm lifts to 0.7 of the forearm's angle from hanging down, past overhead when the forearm
  points up and back, so the wind-up's hand is above the head. The arm turns from rest into the
  swing over the first 600 of phase (two updates of `KF_WEAPON_ATTACK_PHASE_STEP`) and back over
  the last 600, each part the short way round.
- `preview.py swing W` draws every update of an attack, `preview.py rest W` the knight at rest from
  round about and below.

## The stance

Standing, the knight is ready: the left foot 9 degrees ahead and the right 7 behind, knees soft
(9 and 13), feet 6 degrees apart, the free arm 8 forward and 7 out with its forearm bent 30. It
breathes: every 3.6 seconds of game time (`host_clock_ns`) the torso leans 0.8 degrees and the
arms rise 1.5, each player at its own point of the breath. The walk takes over as the stride grows,
the stance and breath fading by the same amount.

## The walk

The game's walkers are morph clips, sampled linearly between keyframes; the model was the
skeleton (B1 actor model 5, its move clip 1), measured from its vertices. One leg, over the
4096-phase cycle, from its foot leaving the ground:

| Phase | Skeleton thigh / knee | Knight thigh / knee |
| --- | --- | --- |
| 0 (foot leaves the ground behind) | -15 / 11 | -22 / 14 |
| 1024 (end of the swing, reaching) | 40 / 46 | 42 / 50 |
| 2048 (foot lands) | 28 / 18 | 30 / 12 |
| 3072 (mid-stance) | -1 / 20 | 4 / 16 |

Degrees, thigh forward of vertical. The thigh swings forward in a quarter of the cycle to a
reach past the landing angle, settles onto it, and sweeps back through the stance over half the
cycle; the feet barely lift (the skeleton's by about 12 units). The other leg is half a cycle
behind. The knight's range is a little wider than the skeleton's, since a player runs 180 units
an update and the skeleton walks 110; the left arm swings against the right leg (0.4 of its swing)
with the forearm bent 12 degrees, and the weapon arm holds its rest.

- The body drops so the longer leg stands on the floor (by 9 to 81 units through the cycle,
  lowest when both feet are down), as the skeleton's hips bob by about 33.
- The cycle advances by the distance a player moves between poses (`port/ext/online_stride.h`,
  unit-tested): 1700 units a cycle, the planted foot's sweep, so the feet keep pace with the
  floor. The skeleton's slide: it covers 1760 a cycle on a 1200 sweep. Moving backwards plays it
  backwards; strafing plays it forward.
- The stride's size follows the speed (full at 150 an update), eased half way each update, and is
  held through two updates that repeat the last, which is what the playback shows when their poses
  run out for a moment. A change of floor or a move over 1000 units leaves the player standing.
- `preview.py walk W` draws the knight's cycle above the skeleton's at the same phases, side on
  and three-quarters.

## In the game

- The knight's TMD and joints are `port/ext/online_player_model.cpp` (generated), behind
  `kf/platform/online_players.h`. The game draws each part with the actor lights, selecting the
  knight as the current TMD; nothing takes an asset slot.
- The pose (`port/ext/online_pose.h`) is version 6, 34 bytes: floor, position, yaw, the equipped
  weapon, the attack phase, the camera's pitch, the update it was sent in, the feet's height,
  whether the player is down or in a fight ([COOP_PLAN.md](COOP_PLAN.md), "Death"; a player who
  is down is not drawn) and a count of each of the knight's sounds; then the player's spells in
  flight ([COOP_PLAN.md](COOP_PLAN.md), "Other players' spells and sounds").
  `game_world_update` sends one every update (`kf::online_update`), in the main loop and while
  the player is held in a menu or a scripted loop, so a room sends 12 to 30 a second, the tick
  rate.
- **Playback** (`port/ext/online_playback.h`, unit-tested): poses are kept by their update and
  played one per update of this machine, the same rate since a room has one tick rate. It runs
  100 ms behind the newest heard (at least two updates) to ride out late poses; it holds the last
  pose when they run out and waits for the delay to build again, plays two an update when more
  than one past the delay, and jumps when 32 past it (a stall). A lost pose is filled in between
  its neighbours (position, height, the facing the short way round, pitch), but not across a
  change of floor or a warp. A swing advances 300 an update (`KF_WEAPON_ATTACK_PHASE_STEP`), so
  any pose inside it places every update of it: a swing is not sent as an event of its own. The
  attack event that lets its target feel it is coop's (`COOP_PLAN.md`, "Hits across machines").
- The knight stands on the sent height plus the drop of its gait, so it climbs a stair and falls
  as its player does. The enemies take another player at that height less the camera's 1500,
  as they take this player at its camera.
- The head bends about the neck by the player's pitch, so others see where they look up or
  down; held to 60 degrees, a neck's reach, though the look limit allows 85.
- A remote player's weapon file is loaded the first time someone holds it (16 at most, about
  25 KB each) and its hold found once; through a swing it is sampled into the morph scratch as the
  game samples its own, for the three followed vertices. A player with no weapon keeps the knight's
  own hand.
- Checked 2026-10-27 with two headless instances in a room at 15 updates a second: after the
  first two, every one of the other's updates was played, two behind, through a walk and a swing
  (all 13 of its updates, 300 to 3900), and on through a menu held open on this side. The
  renderer's frame smoothing blends the knight between updates, as any model it draws.
- A knight costs about 100 µs to pose and enqueue on the desktop (RelWithDebInfo, headless,
  holding a weapon and swinging, the weapon file's load left out). Not yet profiled: the weapon's
  morph and the eleven parts' lights are the likely bulk.

## Next

1. The room's size on the weakest platform: the knight's cost there, measured once a build runs
   on the 3DS (`RUNTIME.md`).
