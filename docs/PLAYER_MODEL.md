# The online player's model

Other players in an online room are drawn as a knight of our own, made to look like the game's
models, holding the player's real weapon and swinging it with the game's own animation
(`src/game/online_player_render.cpp`, in `patches/src/game.patch`). Not finished: the legs do not
walk yet, and a player on a stair is drawn on the cell's floor.

`tools/player_model/` holds it:

| File | What it is |
| --- | --- |
| `kfmodel.py` | Reads the game's formats (MIX chunk streams, model archives, assets with morph clips, TMD, TIM) and renders a model to a picture, culling faces as the game does. |
| `body.py` | Builds parts from lofted cross-sections, boxes and panels, and writes them as a TMD. |
| `knight.py` | The knight's part table and the rig that poses it. |
| `arm.py` | The weapon arm: a first-person weapon model on the knight's elbow, through an attack. |
| `preview.py` | The command: `survey`, `floor N`, `knight`, `swing W`, `gltf`, `tmd OUT`, and `cpp`, which writes the knight into `port/ext/online_player_model.cpp`. Pictures go to `build/player_model/`. |
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

529 vertices in 11 parts (pelvis, torso, head, upper arms, forearms, thighs, shins), each its
own TMD object with its vertices relative to its pivot, about 1850 tall (the player's collision
height is 1700). Grey metal from page 151 for the armour, page 142's darker metal for the
gauntlets so both arms match the weapon arm, leather belt and boots. Tabard, sleeves, knee
bands and crest are in the player's colour, to tell players apart.

## The weapon arm

`WEPON/WEPxx.MIM` (one asset each, loaded into asset 20 for the equipped weapon) is not a
weapon alone: it is a forearm and gauntlet holding it, 200 to 370 vertices, with one clip (six
keyframes for 4096 of attack phase; seven for the Colichemarde's thrust). Its origin is the
elbow, which moves under 230 units through a swing. All its textures are on pages 140 and 142,
both common, so any player's weapon can be drawn on any floor.

On the knight, the remote player's right forearm is that model:

- scaled by 0.5 (the first-person forearm is about twice a body's), at the end of the right
  upper arm;
- turned by the weapon table's own first-person rotation (`COM/COM.DAT` chunk 2, 44-byte
  records, yaw mirrored on load) and a half turn, since the camera looks along +z and actors
  face -z; the table's translation and projection distance are tuned for the view and unused;
- posed by its clip at the peer's attack phase, so others see the swing its player sees.

At rest the arm holds the clip's last frame, a low guard with the blade at the side, which is
also where every swing ends. An attack blends the guard into the swing's first frame over 600
of phase (two updates of `KF_WEAPON_ATTACK_PHASE_STEP`), and the upper arm lifts forward 0.9
radians until the hit (phase 3072, the Colichemarde's 1000), then lowers by the end.

Known flaw: the blend moves vertices in straight lines, so on its first update the arm briefly
folds toward the body.

## In the game

- The knight's TMD and joints are `port/ext/online_player_model.cpp` (generated), behind
  `kf/platform/online_players.h`. The game draws each part with the actor lights, selecting the
  knight as the current TMD; nothing takes an asset slot.
- The pose (`port/ext/online_pose.h`) is version 2, 17 bytes: it adds the equipped weapon and the
  attack phase, which the game publishes every update (`online_publish_weapon`).
- A remote player's weapon file is loaded the first time someone holds it (16 at most, about
  25 KB each), sampled into the morph scratch as the game samples its own, halved, and drawn on
  the elbow; a player with no weapon keeps the knight's own forearm.
- The phase arrives 20 times a second and is drawn as it last arrived, so a swing shows about
  nine of its fourteen updates.

## Next

1. Walking: legs and left arm swung by distance moved, from the change in position.
2. Advance a remote swing between poses at the game's rate, so every update of it is drawn.
3. The height: the pose has none, so a player on a stair or in the air is drawn on the cell's
   floor.
