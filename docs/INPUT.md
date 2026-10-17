# Input: the pad, the keyboard and the mouse

Verdite3's `docs/INPUT.md` is the model. What this game reads, the layout the port
ships, and mouse look.

## The pad

Stage A reads its pad word once a tick: `func_8005012C(1)`, returned at
`0x80018900`, in the libetc layout (`0x0001` L2, `0x0002` R2, `0x0004` L1,
`0x0008` R1, `0x0010` Triangle, `0x0020` Circle, `0x0040` Cross, `0x0080` Square,
`0x0100` Select, `0x0800` Start, `0x1000` Up, `0x2000` Right, `0x4000` Down,
`0x8000` Left). The previous tick's word is kept at `0x80057B30`, and the one-shot
verbs are taken on the press (the bit set now and not then). Read from the code
(2026-10-05):

| button | what stage A does |
|---|---|
| Up, Down | walk |
| L1, R1 | strafe |
| Left, Right | turn: the yaw velocity `0x800A0846` |
| L2, R2 | look: the pitch velocity `0x800A0848` |
| Triangle | attack (`func_80016B24`; the arm swings, measured) |
| Circle | examine (`func_80034DE4`: save points, doors, signs) |
| Square | uses the held item or spell (reads `0x800A07E4` and `0x800A07FA`; unnamed) |
| Cross | the in-game menu (`func_80036E38`) |
| Select | read as Cross |
| Start | `func_8001B7B0(3)`, a message |

**Turning and looking are Verdite3's three branches**, inline in stage A: Left
adds `rate >> 2` to the yaw velocity and clamps it at `rate` (the s32 at
`0x80057E70`), Right subtracts, neither decays it by `rate >> 2` towards 0; then
`yaw = (yaw + vel) & 0xFFF`. R2 adds 3 to the pitch velocity (clamped at 10), L2
subtracts, neither decays it by 2; then the pitch takes it and is clamped to
±`0xBF` (about 17 degrees) as a signed angle, not a 12-bit one.

## The keyboard layout

`patches/KeyLayout.cs`, Verdite3's mechanism (Verdite Core's `KeyLayoutApply`):
**W/S** walk, **A/D** strafe, the **arrows** turn, **Space** attack, **F**
examine, **Q** the held item or spell, **Tab** the in-game menu, **Enter** Start,
**Right Shift** Select. **L2 and R2 are left unbound**, as in Verdite3, because
pitch is the mouse's. It is the port's default, not an override: `Configure`
runs before the config loads, so a fresh install gets it and `settings.json` wins
after; `Install` migrates a stock config once, recorded as `kf1.keys.layout` in
`interface.ini`. Measured: a config with no bindings boots with the layout and
the marker at 1.

## Mouse look

`Mouse` (Verdite Core's, with this game's values in `MouseLook.Game`) collects the
motion; `patches/MouseLook.cs` spends it in a **post hook on stage A's pad read**
(`func_8005012C`, return address `0x80018900`), before any of stage A's tests.
For an axis the mouse moved, it masks that axis's buttons out of the word stage A
is about to test and writes `step ± decay` into the velocity, so the no-button
branch lands on the step exactly, through the game's own clamps. The tick after
a mouse-driven tick gets a zero velocity, so the view does not coast when the
hand stops. Fractions are carried from tick to tick. Since 2026-10-17 the
hook hands the word to `Analog.Tick`, which spends the mouse and the sticks
together ("Analog sticks" below); the mouse's steps are unchanged. 0.15° a pixel at
sensitivity 1 (51 yaw units for 30 pixels).

The buttons press pad buttons through `PadReadEvent` while the pointer is
captured: **left Triangle (attack), right Square (item or spell), middle Circle
(examine)**. **Escape** captures and releases (`KF1_MOUSE_KEY`).

**The mouse leads the tick**: `ViewSmoothing` replaces the lerp's share of the
last tick's mouse turn by all of it and adds the motion not yet spent, Verdite3's
lead, with the pitch kept signed and inside ±`0xBF` (the map walk takes a special
stencil past ±511, so a 12-bit pitch must never reach it). Instant mouse look
(`KF1_MOUSE_LEAD`) gates it.

### Measured

2026-10-05, with a synthetic hand (a local hack, not committed: a fixed step a
tick, on for two seconds of three, fed past `Mouse.TakeLook`):

- 51 and 7 a tick at 144 fps: **yaw stepped exactly 51 every tick**; pitch 7 a tick
  until it **stopped at the limit, 191** (`0xBF`), and stayed there.
- -37 and -5 at 60 fps: exactly -37 and -5 a tick.
- **The hand stopping**: yaw and pitch held on the next tick and every tick after
  (2040 and 191; 3585 and -70), with no coasting.

**Not measured**: the lead (it reads the captured pointer, which the hack did not
provide), the buttons. **Not judged by eye**: the pitch direction (mouse down is
pitch up the velocity R2 raises; whether that looks down), sensitivity, the feel.

## Analog sticks

`patches/Analog.cs`, ported 2026-10-17 from Verdite2's current version, the one
built to keep the original's speed: a stick at full throw turns, looks and walks
at the rates the D-pad and L2/R2 do, never faster, and less in proportion. It
runs in the same hook as the mouse, and on each axis does what the mouse does
with the game's own branch: for a step inside the clamp it sets the axis's
button and writes `step ∓ accel` into the velocity, so the held branch lands on
the step; past the clamp it sets no button and writes `step ± decay`. Every
axis is stage A's three-branch form (addresses in "The player" in
`docs/GAME_INTERNALS.md`):

| axis | velocity | + button | − button | accel | decay | clamp |
|---|---|---|---|---|---|---|
| turn | `0x800A0846` | Left | Right | rate>>2 | rate>>2 | rate (28) |
| pitch | `0x800A0848` | R2 | L2 | 3 | 2 | 10 |
| walk | `0x800A0842` | Up | Down | speed>>2 | speed>>3 | speed (180) |
| strafe | `0x800A0840` | R1 | L1 | speed>>2 | speed>>2 | speed |

**This game's difference from Verdite2**: pitch and walk decay by less than they
accelerate (Verdite2's axes decay by their accel), so `Ramp` and `Drive` take the
decay separately.

The design, as Verdite2 has it:

- **2D** (the default): the right stick turns and looks, the left walks and
  strafes. **3D** (`KF1_TANK=1`): the left stick walks and turns, with a cross
  deadzone of 0.3 and a turn curve of 2; the right stick is not read.
- The right stick's turn and look ramp in through the game's own accel (`EASE`,
  on), and on release the game's own decay brings them to rest. Look
  acceleration and instant stop are off. Movement is direct.
- A radial deadzone of 0.15; curves 1.35 (look) and 1.0 (move); the fraction
  of every step carried to the next tick.
- The left stick's leak into the D-pad on a real pad is owned: while the left
  stick drives, its D-pad bits are cleared, with a zero turn step.
- The mouse takes an axis it moved this tick; a held button takes it from a
  mouse that has stopped.

Switches in `docs/ENV_VARS.md`, kept in `interface.ini` (`kf1.analog.*`) from
the Testing tab's "Analog sticks" section, which also shows the sticks.
`AnalogProbe` (`KF1_ANALOG_PROBE=1`, or `trace` for a line a tick) checks it.

### Measured

2026-10-17, with the shell's `stick` verb and no pad connected, from a New Game:

- **Right stick full right**: yaw steps 7, 14, 21, then 28 a tick, the D-pad's
  ramp and rate. On release the game's decay took it to 3676 from 3718, the
  42 predicted, and the velocity to 0.
- **Half throw** (0.5): steps of 8 and 9, mean 8.6 against the 0.306 × 28 = 8.58
  that the deadzone and the curve predict.
- **Right stick full down**: pitch steps 3, 6, 9, then 10, stopping at 191.
- **Left stick back**: 180 a tick (|(−156, −93)| = 181.6, the sqrt's rounding).
- **The leak**: D-pad Left held while the left stick strafed turned 0; D-pad Left
  alone still turns.
- **From `area1-start`, one second each**: the stick strafed 2975 and R1 2702,
  the stick walked 3348 and Up 3082, in the same directions. The differences,
  273 and 266, are the D-pad's three ramp ticks (45 + 90 + 135 = 270); the stick
  is direct.
- **3D**: the left stick sideways turned 7, 14, 21, then 28; at 0.6, 5 a tick
  (5.1 predicted); up walked 180 a tick; the right stick did nothing.

**Not measured**: the mouse through the merged path (it needs a synthetic
mouse); a real pad's leak (the synthetic stick sets no D-pad bits). **Not
judged by eye**: the pitch direction (stick down is R2's, as the mouse's down
is), the feel and the defaults.
