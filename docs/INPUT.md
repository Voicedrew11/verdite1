# Input: pad, sticks, keyboard and mouse

**How the game reads input at all.** It never calls `libpad`. It reads the pad
through the BIOS (`B(16h) PAD_dr`) via libetc's `PadRead(id)`, which hands back
`~*(u_long*)buf` — so the word is active **high**, and its two button bytes are in
the opposite order to the runtime's `Controller` bit layout. Stage 3 of the main
loop stores that word to `0x80199554` once a frame and every consumer reads that
global; the area modules never touch it, so all player control is `GAME.EXE` code.

**Nothing here hardcodes a pad bit.** The control code tests `pad & mask[i]`
against a 24-word action-mask table at `0x8006E568`–`0x8006E5D0`, which is what
makes the game's own control-config screen work — so the port reads the table too,
and gets the byte order right by construction by ORing the game's own mask words
back into the game's own pad word. The table, its decode and the sign conventions
are in "The action-mask table" in [GAME_INTERNALS.md](GAME_INTERNALS.md), and the
three-branch velocity shape every axis has is in "Every control axis has the same
three branches" there — that shape is what makes all of this cheap.

**Two corrections are recorded below rather than quietly fixed**, because both are
the same mistake: the mask table names the *button* behind an action and says
nothing about what the action is, or which way an axis runs. A plausible story
about a branch is not evidence. The pitch sign and the attack/use pair both had to
be settled by playing the game.

## Analog twin-stick control

`patches/Analog.cs` (it began as a mod) gives the game continuous analog turning,
looking and walking on a modern layout — left stick walks and strafes, right stick
turns and looks — and it does it **without replacing any of the game's own
movement code**.

The sticks were always there: `InputManager` fills
`Controller.LeftX/LeftY/RightX/RightY` from SDL every poll. Nothing consumed
them, because the game reads `PAD_dr` and gets a digital word, and the runtime's
default binding just wires the left stick to the D-pad
(`GamepadBindings.Up = [11, 104]`) — which in this game means the left stick
*turns*, at the fixed rate, like the D-pad does.

**The trick is the three-branch velocity shape** documented under "Every control
axis has the same three branches" in [GAME_INTERNALS.md](GAME_INTERNALS.md).
Because each axis is `vel += rate>>2` on a held button and then
`angle_or_position += vel`, a hook can pre-load the velocity word with
`target - accel` and assert the matching button in the game's pad global: the
game's own next instruction adds `accel`, lands exactly on `target`, and then
applies it through its own path. Collision, the pitch limit, the walk
normalisation, footsteps and animation all run untouched, on an amount the stick
chose. Nothing is replaced, no `HookManager.AddReplace` — two pre-hooks, on
`func_80028DB8` (turn/look) and `func_800290D4` (walk/strafe), plus one post-hook
on stage 3 (`func_8002A550`) for the probe.

The clamps are never fought: with `|target| ≤ rate` and the button asserted in
the direction of `target`'s sign, the pre-loaded value is always inside `±rate`
after the game's own accumulate, so the clamp branch is not taken.

Four things worth keeping:

* **The camera accelerates while the stick is held out.** A d-pad is down or it
  is not, so the game has no notion of a ramp; a stick held at the edge for half
  a second should be sweeping faster than one just pushed. The patch ramps a
  look-speed multiplier to 2.2× over half a second past 80% deflection and drops
  it three times faster, which — with the cap lifted above — is what took the
  camera from "stiff" to something like a modern shooter's. Fine aim near centre
  is untouched, because the ramp never starts there.
* **The fractional carry is not optional.** A small deflection rounds to a zero
  step every tick; without carrying the remainder the player simply does not move
  below about a third of stick. The world ticking at 20 rather than 30 makes this
  worse, not better — there are fewer chances a second for a remainder to cross 1.
* **Buttons come from the mask table, never hardcoded**, so the patch follows the
  game's own control-config screen — and gets the byte order right by
  construction, since it ORs the game's own mask words back into the game's own
  pad word.
* **The left stick leaks into turning** unless the turn masks are taken away from
  it, because the runtime binds the left stick to the D-pad and the D-pad *is*
  the turn control. Measured before the fix: 168 yaw steps in 300 frames with
  the right stick idle. The patch therefore owns the turn bits with a zero step
  whenever the left stick is deflected, and leaves them alone when both sticks
  are centred — so the D-pad still plays exactly as it did.
* **The per-frame speed limit only exists in the two button branches.** The
  branch that runs with *neither* button down decays the velocity by the same
  step and then applies whatever is left, unclamped — so writing `target + accel`
  and asserting nothing lands on `target` however large it is, while writing
  `target - accel` and asserting the button is capped at the game's own rate.
  That is the whole difference between a camera that tops out at the d-pad's
  74°/s and one that does not, and it costs one branch in `Drive`. Confirmed in
  play: mean yaw steps of 39 against a frame rate limit of 28, and a `turnVel` of
  37 sitting in memory, which no button can produce. Nothing else gameplay-side
  reads the turn masks — the only other readers are the control-config screen —
  so dropping the button for a frame is free.
* **A released velocity ramps down, and on a stick that reads as inertia.** The
  game drops nothing: pitch decays by 3 a frame from a limit of 32, so releasing
  the stick keeps the view moving for about eleven frames — a third of a second,
  some 16°. That is reasonable for a button, which cannot be released halfway,
  and wrong for a stick, and it showed up *only* on pitch because the leak fix
  above was already zeroing turn whenever the left stick moved. The patch therefore
  drives a released camera axis to zero for one frame and then hands it back, so
  L2/R2 and the D-pad still work. Movement is deliberately left alone: its
  ramp-down is the walking momentum the game has always had.
* **Sticks idle means the patch is idle.** Both hooks return before touching memory, which
  is what keeps D-pad and keyboard play identical to having it switched off — which is why it can default to on.
* **The look hook is now shared with the mouse.** `Mouse.TakeLook` hands
  `BeforeLook` a step in the same yaw units and this class writes the word, so
  two devices cannot fight over one velocity; the look hook therefore also runs
  with the sticks switched off. See "Mouse look" below.

`KF2_ANALOG_PROBE=1` reports the velocities, the yaw and pitch steps, the walk
speed and turn rate next to the stick deflection that produced them, and dumps
the mask table once. That dump is the evidence for the sign conventions in the
table above — with one gap it cannot close: it names the button behind an action
but not which way the view moves. That cost the first build an inverted look
axis, fixed by playing it; **increasing pitch looks down**. The "Invert look Y"
toggle is now a preference rather than a guess.

### Analog control became a patch, under Input

The fourth conversion, and the reason is "What belongs in a mod, and what does
not" applied to the pad: **a controller plugged into the port without this has its
left stick bound to the D-pad, and the D-pad in this game turns rather than
walking.** A player is not going to guess that the fix is a package under `mods/`
that ships disabled. So the mod is now `patches/Analog.cs` plus
`patches/AnalogProbe.cs`, on by default.

Defaulting it *on* costs nothing, and that is a property of the design rather than
a hope: both hooks test the shaped stick vector first and return before reading
game memory, so with the sticks centred — every keyboard player, every D-pad
player — the patch is two float comparisons a frame and the game plays exactly as
it did. The mod could not have shipped that way at all, being off by default.

The mechanical part was the same as the three before it, with one addition:

- `[PreHook]`/`[PostHook]` became `SymbolRegistry.Resolve` plus
  `HookManager.AddPre/AddPost` in an `Attach` deferred to the first
  `OverlayLoadedEvent`, hook bodies became `public`, and `OnLoad`'s env reads
  became `Configure()` from `Program.cs` with the config read moved to
  `RuntimeReadyEvent`.
- **`Configure` reads its own environment** instead of being handed the strings
  the way `NoDither.Configure(probe)` and `AutoReload.Configure(a, b, c)` are.
  Eighteen string parameters would be a worse index of which variable sets what
  than the table in the class comment already is, so `Program.cs` keeps the list
  in its own comment and calls `Configure()` bare.
- Precedence is the same rule, spelled once instead of per knob: `Env(name, key,
  ref value)` records the *key* in a `_fromEnv` set, and `Saved(key, ref value)`
  skips any key in it. Eighteen `…FromEnv` bools would have been the alternative.
- `AnalogProbe` stayed a separate class and kept its own switch, because it is
  the only half of the pair that costs anything when idle — its hook runs once a
  frame whether or not a stick moved.

Verified in a run with the disc: `[KF2] analog: on, 3 hook(s)`, alongside
`pacing: 5 hook(s)`, `dither: 12 hook(s)` and `autoreload`, with no replace
conflict — note that the probe's post-hook and auto reload's post-hook are on the
*same* function (`func_8002A550`, the end of stage 3) and both run, since
`HookManager` keeps a list per function and only `Replace` is exclusive. Writing
`kf2.analog.turn=2.5` and `kf2.analog.lookdeadzone=0.3` into `interface.ini` came
back as `(deadzone 0.3, turn x2.5, move x1)` on the next start, which is the
`RuntimeReadyEvent` read landing before `Attach` — the ordering the whole settings
mechanism depends on. With the probe on, the mask table dumped once and the
windows reported the patch driving the game: `look 185 move 279` frames out of
300, `mean |step| 58` against a turn rate of 28, which is the overspeed path (see
the per-frame speed limit above) working exactly as it did as a mod.

### The page was under the fold, and the fix was the wrapper after all

The page was in the right *section* and the wrong *place in it*. `Extend` draws
after a section's own content, and the input section's own content is two tab
bars, sixteen binding rows and a reset button — around 450px of a 500px window —
so "Analog sticks" started below the fold and was only found by scrolling past
every button in the game. Worse than the scrolling: the two keyboard-layout
buttons *write* the sixteen rows of a table a screen above them, and the stick
settings sat nowhere near the pad one.

Three options were weighed here and the one picked was **a `controls` section of
the port's own**, `Order = 1` so it sits beside Input — a sidebar entry rather
than a hoist, needing no wrapper and no patch to the checkout. **That option is
dead, on a fact nobody had checked: `settings.input` already renders "Controles"
in pt-BR *and* es-419** (`languages.json:398`). A second sidebar entry called
Controls collides head-on with the Input pane's own name in two of the three
languages, giving a sidebar with two identically-labelled rows. The general
finding is worth more than the instance: **a new sidebar entry has to be checked
against every language of the ones already there, not just English.**

So the rejected option was taken instead. **The port wraps the section** —
`patches/settings/InputSection.cs`, `Id => "input"`, and
`SettingsRegistry.Register` replaces by id. The old objection is unchanged and is
now a cost being paid rather than an argument that lost: the port owns a section
the runtime registers, in a checkout that is gitignored and moves under us. Two
things pay it. The wrapper is cheap to abandon — deleting one `Register` line
restores the runtime's pane exactly — and the copied half names its source, so a
pin bump has something to diff. Splitting the pane could not have delivered the
point of the exercise anyway: the layout buttons have to sit with the table they
write, and that is one pane by definition.

The upstream gap is unchanged and is still worth an issue, with a second half
found here: **`SettingsRegistry.Extend` has no ordering argument, and no
un-extend.** The list is append-only with no removal API, which is why the four
pages had to be *dropped from the registry* rather than reordered in it — see
below.

### Open: the camera does not feel consistent in every direction

Reported from play and **not yet diagnosed** — the camera's speed does not feel
even across directions, and not all the time. Nothing below is confirmed; these
are the candidates worth measuring first, and three of them are certain to be
*real* effects whether or not they are the one being felt:

1. **The game turns you slower while you walk.** Stage 3 sets the turn rate to
   `0x1C` (28) when a movement button is down and `0x23` (35) when none is, so
   horizontal speed drops by a fifth the moment you move. The patch inherits this
   because it reads the rate out of `0x8019955C` each frame. This is the best fit
   for "not all the time" and it is the game's own rule, so overriding it is a
   decision, not a fix.
2. **Vertical and horizontal are not the same scale, and the ratio moves.** Yaw
   steps by up to `rate` (28 or 35) and pitch by a fixed 32, so the vertical
   speed is constant while the horizontal one changes with what you are doing.
   Standing still the camera is relatively faster sideways than it is walking.
3. **Diagonals reach full deflection sooner than cardinals.** `AxisToByte` gains
   the axis by 1.3 and clamps *per axis*, so a stick pushed to a corner reads
   (1, 1) — radial magnitude 1.41, not 1. `Shape` renormalises the direction
   correctly, but the deadzone and curve are applied to a magnitude that saturates
   at a different point depending on the angle, which makes mid-deflection
   diagonals proportionally quicker than mid-deflection cardinals.
4. **The acceleration ramp is radial and applies to both axes.** Sweeping hard
   sideways speeds the vertical axis up too, so a diagonal flick during a sweep
   is faster than the same flick from rest.

The measurement to take is the patch's own: hold a fixed deflection at eight
directions in turn with `KF2_ANALOG_PROBE=1` and compare mean |yaw step| and the
pitch total, once standing still and once walking. Candidate 1 will show up as
two clean bands and would settle it immediately.

Measured as a mod, loaded alongside `widescreen` (and the dither patch, then still
`mods/nodither`): `loaded 3/3 mod(s), 9 function(s) hooked`, no replace conflict,
300 frames per 10-second window — the pacing floor is untouched. As a patch the
probe reports the same 300 frames per window, now with the frame pacing, the
dither patch and auto reload all hooked as well.

### Open: the twin-stick controls broke after the dragon stone went into the fountain

Reported from play and **not yet reproduced or diagnosed**. Putting the dragon
stone into the fountain — a scripted world event — and the sticks stopped
working afterwards. Unknown so far: whether it was both sticks or one, whether
the D-pad still worked, and whether it survived leaving the area or a reload.
Those three answers narrow it to one of the candidates below on their own, so
they are worth capturing next time before anything else.

What makes this worth writing down rather than guessing at: it has **two
silent bail-outs that a scripted event is exactly the thing to trigger**.

1. **The rate guards.** `BeforeLook` returns when `*(u32*)0x8019955C <= 0` and
   `BeforeMove` when `*(u32*)0x80199558 <= 0` (`Analog.cs:247,291`). They exist so
   the patch never divides a scale by a zero the game is holding, and the game
   plausibly zeroes exactly these while a scripted sequence has the camera. If
   the event leaves either at zero, that axis is dead until something sets it
   back — and the patch would be *reporting* the game's state faithfully rather
   than having a bug of its own. **This is the first thing to check** and it is
   one read each.
2. **The action-mask table.** `Drive` reads the button masks out of
   `0x8006E568`–`0x8006E5D0` every frame rather than hardcoding them, which is
   what makes it follow the control-config screen. If the event rewrites or
   clears that table, `inc`/`dec` come back zero, no button is asserted, and the
   game takes its neither-button branch — which *decays* the velocity instead of
   accumulating it. That reads as controls that are alive but weak and wrong,
   not as controls that are dead, so it is distinguishable from candidate 1 by
   feel alone.
3. **The player action state.** `0x801994E1` drives the jump table at
   `0x80011300`; states other than the ordinary ones take arms that need not call
   `func_80028DB8` and `func_800290D4` at all. If the event parks the player in
   such a state, the patch's hooks simply never run — and neither does the game's
   own control code, so the D-pad would be dead too. **That is the question the
   "did the D-pad still work?" answer settles**, and it is the one case where
   nothing is wrong with the patch.

Cheap next step: reproduce with `KF2_ANALOG_PROBE=1`, which already reports the
control state, and read `0x8019955C` / `0x80199558` / `0x801994E1` at the moment
it breaks. All three candidates are one memory read apart.

## The Input pane is the port's

`patches/settings/InputSection.cs` and `patches/settings/BindingTable.cs`. One
pane, one tab bar, three tabs:

```
— Input ————————————————————————————————
[ Keyboard ]  [ Gamepad ]  [ Mouse ]

  [King's Field layout]  [RecompOne layout]
  The port's layout is in place. …

  Button    In King's Field        Key
  ──────────────────────────────────────────
  Cross     use, open; confirm     F
  Circle    the in-game menu       Tab
  Square    attack                 Space
  …
  That middle column is what the buttons do by default. …

  [ Reset to Defaults ]
```

Gamepad is the same shape with the twin-stick block and the map's pad button
above the table and the pad column beside it; Mouse is `MousePage` and nothing
else.

**Four things are load-bearing.**

**A page drawn in a tab is a body, not a page.** `IPatchPage.Order` is inert —
the sequence is written out in three small arrays, so there is no list to sort
and no tie to break — and `Title` keeps only its degenerate half, that an empty
title declines the heading. `KeyLayoutPage` and `MousePage` are alone in tabs
that already name them and take `Title => ""`; `AnalogPage` and `MapButtonPage`
share the Gamepad tab and keep theirs.

**The four pages had to leave the registry, not just be reordered in it.**
`SettingsPopup` draws `current.Draw()` and *then* every
`GetExtensions(current.Id)`, and there is no un-extend, so a page still
registered against `"input"` would draw a second time under the tab bar, outside
every tab, permanently and irreversibly. The `Register("input", …)` calls are
gone from `PatchSettings.Install` and `PatchSettings.Register` now refuses that
id with a message naming the page, so the hole cannot be re-opened by a later
patch.

**Replace, do not unregister.** `Register` already removes by id. An explicit
`Unregister("input")` would state removal where the intent is substitution and
would hide the one failure that matters — if upstream renames the id, the
unregister no-ops silently and the register adds a *second* Input tab. That is
warned about directly instead, in the shape the existing section-id check uses.

**The tab bar has to stay out of the scroll.** The table alone is about 490px —
sixteen rows at a frame height plus cell padding, plus a header — against roughly
365px of tab body in a 500px popup, so every tab scrolls whatever is above it.
Drawn in the flow of the settings content child, the bar would scroll off the top
with everything else and the pane would be *worse* than the one it replaces. Each
tab body therefore gets a `BeginChild` of its own that takes the remaining height
and owns the scrollbar, with the padding push around `BeginChild` only — patch
`0031`'s shape — so it is not inset a second time inside a child that has already
padded it.

Two smaller things fell out. The capture row is **one field per device**
(`_keyRow`, `_padRow`) rather than the runtime's single index shared by two
devices and two pad slots, which had to be cleared in four places; two fields
make switching tab mid-capture a non-event by construction. And the Keyboard
tab's Reset routes through **`KeyLayout.ApplyStock()`** rather than writing
`Keys = new KeyBindings()` as the runtime did — the same object, plus the marker
that stops `KeyLayout.Install`'s next-launch migration putting the port's layout
back over it. The "RecompOne layout" button a few lines above does exactly this,
and two buttons that agree on screen have to agree in code; they could afford not
to while they were 450px apart.

### The Pad 1 / Pad 2 tab bar is gone

It was a whole tab of bindings that could not reach this game. `BiosB.PadRead`
packs pad 2 into the high half of the pad word and the game keeps only the low
sixteen bits, at `0x80199554` — the global stage 3 stores the pad to and every
consumer reads.

Say it precisely, because two statements are easy to confuse and only one is
true: **the port stopped offering pad 2; the runtime has not stopped having
one.** `InputManager.Poll` still fills `Controller.State2` from `Keys2`/`Pad2`,
and `BindingTable` never reads or writes either, so a `settings.json` that
already carries them keeps them byte for byte. `settings.input.pad` (`"Pad {0}"`)
is the one runtime string that stops being used; it stays in the runtime's table
untouched.

### The table names the action, and that reverses a rule

Sixteen rows reading `Cross`, `L1`, `Triangle` told a King's Field player
nothing, and the port reads the game's own action-mask table every frame. So the
table has a dimmed middle column headed **In King's Field**: walk forward, turn
left, strafe right, look down, attack, cast, the in-game menu, use.

That is a deliberate reversal of "A correction: the mask table names the button,
not the verb" below, which ends by saying the settings page would be lying if it
claimed a verb. **The rule is kept and the column is not a breach of it**, on
three counts: the strings are the game's *measured defaults* (the action-mask
table at `0x8006E568`–`0x8006E5D0`, and `func_8002957C`'s four branches, both
already written up here), the header says which game they are for, and one
wrapped note under the table says the game's own control configuration screen
reassigns them. What the old rule was quietly charging for its silence was a
table nobody could read.

`MousePage` carried the same denial and now points at the column instead of
refusing to say. Three entries are deliberately not verbs: `Start` is blank,
because its branch has never been identified and a guess in a column read as
measurement is worse than a gap, and `L3`/`R3` say *the game does not read it*,
which is a fact a blank would not carry.

**The column is English**, like every other string this port writes — every
tooltip, every note, every combo entry under Video and Gameplay — while its two
neighbours in the header row come from the runtime's three-language table. That
is a choice rather than an oversight: a verb mistranslated in a column a player
reads as measurement is worse than one they can see is the port's, and it keeps
sixteen strings out of `Localization.Merge`.

**Never looked at by eye:** whether three tabs read as three devices rather than
as three settings groups, whether the Gamepad tab — which stacks the whole
twin-stick block above the table — reproduces the fold problem it was built to
fix, and whether the action column is worth its width.

## The keyboard layout, and changing a default RecompOne provides

`patches/KeyLayout.cs`. RecompOne's default keyboard bindings are a *console's*
defaults spelled on a keyboard — face buttons on Z X A S, shoulders on Q W E R,
the D-pad on the arrows. That is the right generic answer for a runtime that has
to run any PS1 game and the wrong one for this one, because King's Field walks
**and turns** on the D-pad: the arrows alone are a tank control, and mouse look
in the other hand has nothing sensible to do.

Read through the action-mask table, the pad means:

| pad | action | pad | action |
|---|---|---|---|
| Up / Down | walk forward / back | L1 / R1 | strafe left / right |
| Left / Right | turn | L2 / R2 | look up / down |
| Square | attack | Circle | the in-game menu |
| Cross | the action button: use, open | Triangle | cast |

which rearranges into the layout every first-person game has used since Quake:

```
W A S D  walk and strafe        mouse    turn and look
arrows   walk and turn                   left attacks, right casts, middle uses
Space    attack     F  use      Q cast   Tab  menu
```

**Pitch is the mouse's, and only the mouse's.** The game looks up and down on
L2/R2 and a pad still does, but a keyboard pair for it buys a worse version of
something the mouse does continuously, so the layout leaves those two bindings
empty rather than spending two more keys on them.

### Yes, and it needs no patch to the checkout

`ConfigManager.Game` is public, `Keys` is a settable property and `SaveGame()` is
public, so the port can decide its own bindings. **What matters is when.**

* **`Configure()` runs from Program.cs, before `ConfigManager.Load`.** Load either
  overwrites `Game` from `settings.json`, or — when there is no such file —
  *saves the object it finds in memory*. So writing the bindings before it makes
  them the port's **defaults**, in exactly the relationship RecompOne's own
  defaults have with the file: a fresh install gets them, and after that the
  player's file wins. Nothing is overridden and nothing needs a marker.
* **`Install()` migrates an existing `settings.json`, once.** Anyone who has
  already run the port has a file full of stock bindings, and a default that only
  reaches new installs is not much of a default. The rewrite is guarded twice: it
  happens only if every one of the sixteen bindings is *exactly* stock (one key
  someone chose stops it), and it records `kf2.keys.layout=1` in `interface.ini`
  so that a deliberate return to stock is not undone on the next launch.

Measured, by running the port against a config in each state:

| config | marker | result |
|---|---|---|
| stock | unset | migrated — `Up` becomes `W` |
| a layout the port shipped before | older | migrated to the current one |
| stock | set | untouched: the player asked for stock |
| stock with one key changed | unset | untouched: it is a customised file |
| none at all | — | Load saves the port's layout as its defaults |
| any | — | `KF2_KEYS=fps` overrides both guards |

That second row is not hypothetical: version 1 of the layout had attack and use
swapped (see "A correction: the mask table names the button, not the verb"
below), and fixing it meant bumping
`Version` to 2 and recording v1 in `Superseded`. Without both, a config carrying
the old layout reads as *customised* and would never be corrected. This is the
one piece of bookkeeping a changed default costs.

The marker lives in `interface.ini` rather than in `settings.json` because
`settings.json` is the thing being migrated, and a flag inside it would mean
growing the runtime's own config schema.

**A third thing drifted with that version bump and was not caught until the pane
was rebuilt**: `KeyLayoutPage`'s tooltip still described v1. It said *"R and F
look up and down"* and *"E uses"*, while `Layout()` has `L2` and `R2` empty —
pitch is the mouse's alone — `Space` on Square and `F` on Cross. The console line
`Install` prints was correct the whole time, which is what the fix was checked
against, and that is the general lesson: **the bookkeeping a changed default costs
is `Version` and `Superseded` *and every sentence that describes the layout*.**

**This could not change what the runtime's own "Reset to defaults" button reset
to** — `new KeyBindings()` inside `InputSettingsSection`, unreachable without
patching the checkout to hold an opinion about one game — so the port added its
own pair of buttons instead (`Kf2.Settings.KeyLayoutPage`): "King's Field layout"
and "RecompOne layout", beside the table they both write. Now that the port draws
the pane, that reset is the port's too and goes through `KeyLayout.ApplyStock()`,
which writes the same bindings **and** the marker — so the button and the
"RecompOne layout" button a few lines above it can no longer disagree about
whether the next launch migrates the config back. See "The Input pane is the
port's" above.

### The second binding the schema cannot hold

`KeyBindings` is one key per button — a `string`, not a list, unlike
`GamepadBindings`, which is `int[]`. So binding W to Up *takes the up arrow off
it*, and the up arrow is how the in-game menu moves: a straight WASD swap leaves
menus scrolling on W and S while their horizontal movement is still on the
arrows.

The port adds the arrows back the same way the mouse buttons arrive — by ORing
the bit into the word inside `PAD_dr`, where nothing asks which device set it.
Up and Down therefore have two keys each, which the binding table cannot show and
does not need to. It costs one keyboard poll a millisecond, not one per `PAD_dr`,
and it only runs while the port's own layout is the one in place: a player who
went back to stock, or who bound the arrows themselves, has already said what
those keys do.

The bit order is the same swap the mouse buttons use, and the mask dump confirms
it for this pair specifically: `0x8006E590` reads `0x1000`, which is
`Controller.Up` (`0x0010`) with its halves exchanged.

## Mouse look

`patches/Mouse.cs` steers with the mouse, and presses pad buttons with its
buttons. It is **on by default** and its knobs are under Input, below the stick
ones. Capture is still Escape, so the pointer does not disappear into the game
until the player asks.

**The look half is not a hook.** A mouse and a stick are two ways of asking for
the same thing — the per-frame turn and pitch step — and `Analog.BeforeLook`
already owns that word: it pre-loads the velocity the game is about to accumulate
into and asserts the matching button out of the game's own mask table. So the
mouse hands that hook a number and nothing else, added to the stick's term before
a single `Step` call rounds and carries the remainder. Two devices, one decision,
one write. The one change to the surrounding logic is that the hook now runs with
the sticks switched *off*: keyboard-and-mouse is a scheme of its own, not a
variant of the pad one.

### Three things a mouse is not

* **It is displacement, not a rate.** A stick deflection says "turn at this speed
  while I hold it"; mouse motion says "turn by this much, once". Everything on
  the stick page that shapes a held rate — deadzone, response curve, the
  acceleration ramp — is therefore absent here and deliberately so, and neither
  feeds nor reads the ramp.
* **Its release is not optional.** The game ramps a released look velocity down
  by 3 a frame from a limit of 32 — about eleven frames. On a stick that is a
  defensible feel and is the `CameraInstantStop` option; on a mouse the hand has
  stopped and the camera has not, so the axis is put down the frame motion stops
  whatever that setting says. Hence the second pair of ownership flags in
  `Analog` (`_mouseTurn`, `_mousePitch`): they exist to make the release ignore
  the stick's option, and to hand the axis straight back to the D-pad and L2/R2
  one frame later.
* **The pointer runs out of desktop.** An absolute pointer stops at the edge of
  the screen halfway through a turn, so motion is only motion while the cursor is
  locked to the window. That lock is the only part of this that the runtime had
  to grow — `patches/recompone/0017`.

### The angle scale, and where it comes from

Yaw is 12 bits to the circle (`yaw & 0xFFF`), and the game's own numbers confirm
it: the D-pad's turn rate is `0x1C` **a logic tick**, which is 74°/s against the
30 Hz world this was measured with and 49°/s against the 20 Hz one the port now
defaults to. It is a tick rate and not a frame rate: drawing faster does not turn
faster. Pitch is in the same units, held
inside ±`0x2BC` — about 62° either side of level.

The default is **0.15° a pixel**, so a quarter turn is about 600 px of movement
at sensitivity 1. Window pixels, not mouse counts: the host reports the pointer
in the window's own space, so a larger window turns slightly slower for the same
movement of the hand. Raw cursor mode takes the desktop's pointer acceleration
out of it, but not that.

Two ceilings the stick does not need:

* **`StepCap`, 1024 yaw units (a quarter turn) a frame.** The stick's ceiling is
  four times the game's own per-frame rate, which is generous for something
  asking for a *rate* and much too small for a flick, which arrives as three or
  four very large frames. A cap is still wanted: a mouse knocked off the desk
  should not spin the view eleven times.
* **`StaleMs`, 250 ms.** The accumulator fills whenever the pointer moves and is
  only spent by a routine that runs while the game is walking around. The in-game
  menu blocks inside its own call, an area load takes seconds — and coming back
  out with every pixel moved in the meantime still queued would swing the camera
  through whatever the hand did while the player was reading. Motion older than
  this is dropped rather than applied.

### The buttons go through PAD_dr, and that is the whole design

`PadReadEvent` fires inside the BIOS's `PAD_dr` and its `Buttons` field is read
back, so a held mouse button is ORed into the word the game is *about* to read.
That is worth more than a hook would be:

* it needs no address and no mask — nothing here knows what "attack" is;
* the **game's own control-config screen** decides what the button does, exactly
  as it does for the pad, so remapping in-game moves the mouse with it;
* it works in the menus, on the title screen and anywhere else the game reads the
  pad, none of which the control hooks reach.

The buffer is active low and carries the two button bytes the opposite way round
from `Controller`'s layout (libetc hands the game `~buffer`), which is the same
swap the mask table is stored under — so pressing a button is *clearing* the
swapped bit. See `AnalogProbe.Buttons` for the same rule stated from the other
end.

The listener is attached when the pointer is captured and dropped when it is let
go. `PAD_dr` is the busiest call in this game — every screen transition
busy-waits on it, hundreds of thousands of times a second — and a listener on
that bus is a cost every player would pay for a device most of them are not
using.

**Which pad buttons the defaults are, and why.** `func_8002957C` — the routine
`NOTES` already listed as "player control: attacks, items, magic" — reads exactly
four entries of the action-mask table, against the pad word at `0x80199554` and
the previous frame's at `0x80199556`:

| mask word | entry | button | tested | what the branch does | what it is |
|---|---|---|---|---|---|
| `0x8006E570` | 2 | Square | just pressed | `func_800262C8(0)` | **attack** |
| `0x8006E568` | 0 | Cross | **held** (this frame and last) | subtracts 500 a frame from two `u16` counters at `0x8019942E` and `0x80199432`, clamping at 0, and sets the state byte `0x8019941F` to 1, or to `0x28` when a counter reaches zero | **the action button** |
| `0x8006E574` | 3 | Triangle | just pressed | `func_80027DC0`, which indexes 26-byte records at `0x8019C5EC` and compares `0x8019942C` against the record's `+0x16` before it runs | **cast** |
| `0x8006E578` | 4 | Select | just pressed | the same routine on a second slot, plus `func_800197D4` / `func_800474D0` through a table at `0x8009B52C` | the second slot |

So the defaults are **left = Square, right = Triangle, middle = Cross**, and
Circle is deliberately not among them — it opens the in-game menu (entry 1,
`0x8006E56C`), and a menu on a mouse button under a captured pointer is a trap.

### A correction: the mask table names the button, not the verb

The first version of this shipped with **attack and use swapped**, and the way it
went wrong is worth keeping.

The branches above are real and were read out of the emitted code. The column on
the right was not: it was inferred. A button tested *while held* that drains a
pair of counters reads like a swing and its charge; a single press calling one
routine reads like using what is in front of you. Both readings are plausible and
the second one is backwards — **Square attacks and Cross is the action button**,
which one minute of play settles and no amount of reading the branch does.

That is the same failure the analog patch had on the pitch sign, in the same
place: the mask table gives the button behind an action and says nothing about
what the action *is*, and a plausible story about a branch is not evidence. What
the counters at `0x8019942E` and `0x80199432` actually are is now unidentified
again — they are not a swing charge, whatever else they may be.

It is also why the settings page says "Left button → Square" rather than "Left
button → Attack". The port presses a pad button; the game's own control-config
screen decides the verb, and the port would be lying if it claimed otherwise.

### Capture, and getting out again

Escape, by default. The game's own keymap is Z X A S Q W E R F G, Enter, right
shift and the arrows, and the host claims F1 and F11, so Escape is both free and
where a hand goes to get a pointer back. Three things release it:

* pressing the key again;
* opening any popup — settings, the mods list, the disc picker are all drawn over
  a running game and all want a cursor. This is the one state change nothing
  announces, so it is polled (throttled to 1 ms) off the pad listener, which is
  the one thing the game keeps doing wherever it is;
* switching mouse look off in the settings page.

The key comes off `KeyboardEvent` rather than being polled through
`HostWindow.IsKeyDown`, unlike the debug mod's hotkeys, and that is not a
preference: every hook this port owns is in the walking-around part of the game,
and a pointer captured and then swallowed by the in-game menu has to be
releasable *from inside it*.

### Saying so on screen: a glyph rather than a toast

Capture used to be announced by `ToastNotifications.ShowText("Mouse look", …)` —
a titled card sliding in over the game on every capture, every release, and once
on the first uncaptured motion. It was reported from play as *really annoying*,
and the reason is structural rather than cosmetic: capturing and releasing is
something a player does **while playing**, since it is how you reach the menu bar
and come back, so the notification fires often and every firing is a modal-looking
card reporting a state the player has just this moment asked for. There is nothing
in it to read.

`patches/MouseIndicator.cs` is what replaced it: a white pixel-art mouse in the
top right of the game picture, faded in over 140 ms, held for 1.1 s and faded out
over 420 ms. **The glyph carries the state and the fade carries the change** —
captured is a solid mouse, released is the same silhouette with a two-cell
diagonal cut out of it, computed from the one bitmap rather than authored twice.
The cut is the universal "off" and so needs no learning, which a filled-versus-
hollow pair would have; that was the choice between them. A one-cell black shadow
sits under the white, because the picture behind it is whatever the dungeon
happens to be and a torch-lit wall is bright enough to lose a white shell — and
the cut only reads as a cut if something separates the two halves.

Three things follow the map's viewports rather than being invented again. It is
an `IFloatingPanel` anchored to **`MapRender.Picture`**, the game picture rather
than the window (`patches/recompone/0029`), so it does not sit over the port's
menu bar or in the letterbox bar beside a 4:3 picture. Its cell is a whole number
of screen pixels — `max(2, round(height / 180))`, off the picture rather than off
`Theme.Scale`, since it belongs to the game's image and not to the port's chrome
— because a one-cell outline on a half pixel softens the whole thing. And its
`IsOpen` is `true` with a no-op setter, the shape `MapOverlay` uses: the fade *is*
the open state, `Draw` returns before it begins a window while faded out, and a
setter that wrote anything would give "Reset view" an opinion about a transient.

It is registered from `Mouse.Install`'s `RuntimeReadyEvent` rather than from
`Program.cs`, which puts it after the map's three viewports — `PanelManager` draws
in registration order, and a capture announcement belongs over the map rather than
under it.

**One toast is kept.** "This display cannot lock the pointer" is a failure rather
than a state, it is rare, and it needs words; no glyph says it. The once-a-session
"press Escape" hint is *not* kept, and that is the same argument as the card: a
player who moves the mouse and gets nothing is asking "is this on?", not "which
key is it?", and the cut mouse answers exactly that. The key is still named on the
console line at boot and in the settings page.

**Filming silences it.** The debug mod's cinematic camera suppresses the glyph
outright while it is on (`MouseIndicator.Suppressed`, both ends gated so nothing
queues underneath and switching on mid-fade hides it at once), since a flythrough
being filmed must not have announcements fading in over the picture.

**Never judged by eye**: whether the top right is where the eye is, whether 1.75 s
total is long enough to notice and short enough not to nag, and whether the cut
reads as "released" over a bright scene.

### What the runtime had to grow: `0017`, and later `0032`

`InputManager` owns the `IMouse` and is `internal`, so the port could not reach
the cursor at all. The patch adds three things to it and forwards them from
`HostWindow`, beside the `IsKeyDown` that already plays this role for the
keyboard:

* `MouseCaptured` — `CursorMode.Raw` where the platform has it, `Disabled`
  otherwise. Both make GLFW report an unbounded virtual position; Raw is the same
  thing with the desktop's pointer acceleration taken out. It is a property that
  can be *read back*, because a platform may refuse, and `Mouse.SetCaptured`
  trusts the read rather than the write.
* `TakeMouseMotion` — the accumulated difference between successive `MouseMove`
  callbacks, cleared by the call. It accumulates whether or not anything has
  asked for capture (one subtraction per callback) and is zeroed on the mode
  change, because the pointer teleports when the cursor is locked or released and
  that jump is not motion anyone asked for.
* `IsMouseButtonDown`.

`Shutdown` gives the cursor back, so a crash on the way out does not leave a
hidden pointer behind.

`0032` is the same problem with a much smaller answer, and the port's own binding
table is what wanted it: `InputManager.IsPadConnected` and
`GetFirstPressedPadButton` are **already `public static`** on that internal class,
so unlike the mouse nothing had to be added there — only the two forwards from
`HostWindow`, beside `IsKeyDown` and the mouse block. Two lines against `0017`'s
sixty. UI only, so no recompile.

`0062` is the third of the same shape, and it exists because
`GetFirstPressedPadButton` answers the wrong question for a *bound* key: it
sweeps the enum from zero and returns the first button held, which is what a
binding table's "press a button" prompt wants and is useless to anything asking
about one particular button. The one this port wanted is the highest index a pad
has — SDL's `Misc1`, 15, which is the DualSense's mute key and an Xbox Series
pad's share button, the one button on a modern pad that no PlayStation layout
claims — so anything else held masks it. `InputManager.IsPadButtonDown(button,
pad)` asks about one binding in the same encoding the rest of that class uses, so
the triggers (100/101) and the stick directions (102–109) answer through it too,
and `HostWindow` forwards it. `mods/kf2debug` is the only caller: the mute key
toggles noclip, so a player on a pad never reaches for F3. Input only, so no
recompile.

### What is measured, and what is not

**Measured, and the whole path ran.** `KF2_ANALOG_PROBE=1` now reports the
mouse's mean turn and pitch step per frame beside the stick's, and four
consecutive ten-second windows of real play read:

```
mouse 135 (mean |turn| 72.6, |pitch|  6.3, capture on) | yaw stepped 131/300 frames, mean |step| 74.82, pitch total  820
mouse 191 (mean |turn| 72.0, |pitch| 16.4, capture on) | yaw stepped 181/300 frames, mean |step| 75.85, pitch total 2955
mouse 141 (mean |turn| 61.6, |pitch| 11.1, capture on) | yaw stepped 137/300 frames, mean |step| 63.31, pitch total 1575
mouse 189 (mean |turn| 64.0, |pitch| 10.7, capture on) | yaw stepped 187/300 frames, mean |step| 64.58, pitch total 2017
```

Three claims come out of that, and they are the ones that matter:

* **The frames agree.** 135 frames of mouse motion against 131 frames in which
  the yaw angle actually moved, 191 against 181, 141 against 137, 189 against
  187. The shortfall is the fractional carry holding a small step back a frame,
  which is what it is for.
* **The amount agrees.** The angle the mouse asked for and the angle the game
  applied are within a few percent in every window — 72.6 asked, 74.82 applied;
  61.6 asked, 63.31 applied. So the pixels-to-units conversion, the pre-loaded
  velocity and the game's own accumulate are all landing where the arithmetic
  says.
* **The overspeed branch is doing its job.** `turnVel -70` sitting in memory with
  the turn rate at `0x1C`: no button on the pad can produce that, which is
  `Drive`'s no-button branch — the same mechanism the stick's sensitivity above
  1.0 uses.

Also measured: the host accepts the cursor lock and releases it again on this
machine (`MouseAvailable: True`, `capture accepted: True`, and false again after
the release), the capture key engages it from the real key path, and the four
mask-table reads in `func_8002957C` above are read out of the emitted code rather
than guessed.

**Not measured, and it needs a person to say.** Whether 0.15°/px is the right
*feel*, and whether the pitch runs the right way round — the sign is inherited
from the stick's, and that one the analog work had to fix by playing it, so it is
exactly the sort of thing a counter cannot answer. The mouse buttons have not
been pressed at a door or a monster: the injection is one `&=` on a word the
BIOS is about to hand over, but nobody has watched a swing come out of it.

It is **on by default**. Capture is still Escape: a pointer that would
disappear into the game unasked is worse than one key to find, so look does
nothing until that key is pressed.


### The mouse leads the tick

Reported from play (issue #38, a 360 Hz monitor on Wayland): mouse look *feels
laggy*, and much better with the smoothing off. Both halves of that are right, and
they add up. The look routine runs on the world's 20 Hz tick, so a movement of the
hand waits up to 50 ms to be spent; `FrameSmoothing` then draws
`lerp(prev, cur, phase)`, which reaches the new angle only at the *next* tick. Up
to 100 ms from hand to picture, and a view that coasts on for a tick after the hand
has stopped. For a stick that is the right trade ("The view has to be carried
between ticks" in `docs/PATCHES_AND_MODS.md`): a stick asks for a rate the game
ramps and damps, so only the game can say where the view ends up, and
extrapolating it bounced. A mouse is different in exactly the way that matters. It
asks for a **displacement**, and the game adds it to the angle unchanged: `Drive`
pre-loads the velocity so the accumulate lands on the step, the release zeroes it,
and nothing in between damps it. Measured against the base angle over 40 ticks,
the game applies what was asked to within **0.33 units** of 4,096. So the view can
show a mouse turn the frame it happens, and the tick will agree with it when it
lands.

That is `FrameSmoothing.MouseLead`, inside the stage-8 pair that already carries
the view, so nothing new touches the game's state and `After` still puts it all
back:

* **The host's motion is drained every drawn frame** into a pending sum
  (`Mouse.Poll`, called from stage 8), not once a tick. The look routine spends
  the sum (`Mouse.TakeLook`) exactly as it spent the host's accumulator before.
* **The view is `lerp(prev, cur, phase)` + (1 − phase) × the mouse's share of the
  last tick's turn + the pending sum.** The lerp's own reading of the mouse turn
  is replaced by all of it at once; the stick's and the keys' part still
  interpolates as before. At a tick boundary the pending sum becomes the new
  tick's share, so the two terms hand over continuously.
* **The last tick's share is measured, not assumed.** `Analog.BeforeLook`
  records the base angles and the mouse's fraction of each step
  (`Mouse.NoteSpent`); on the tick frame stage 8 reads the base angles again. So
  the pitch limit, the step cap and anything else the game did to the step are in
  it.
* **Pitch is held inside ±`0x2BC`** off the base angle the next tick will add to,
  so looking into the limit stops at it rather than overshooting and coming back.
  Pending yaw is capped at `StepCap`, as the tick caps it. **The base pitch is a
  12-bit angle, not an s16**: the look routine stores `(pitch + vel) & 0xFFF`, so
  looking just above level reads `0x0FFx`, and only the clamp writes the
  sign-extended `0xFD44`. The first version read it as an s16, so any look upward
  read as about +4,000, was clamped to the *downward* limit, and the view spun
  round to the player's feet. Read both ways it is the same angle mod 4096, which
  is also how the game's own limit test (`func_80015364`, `(a - b) & 0xFFF < 2049`)
  compares them. Measured after, sweeping the full range synthetically: the view
  follows the base from -700 to +700 and is never more than 33 units ahead of it.
* **Nothing is shown unless the look routine is live**: spent within the last
  three ticks, and the world not paused. Motion arriving while it is not (a menu, a
  load, the paused map) is dropped, which is what `StaleMs` did before.
* **With smoothing off it still leads**: the view is the tick's angle plus the
  pending sum. At the tick rate there is no frame between ticks and nothing to do.

`KF2_MOUSE_LEAD=0` is the comparison. `KF2_SMOOTH_PROBE=1` adds a line: frames
led, how far ahead of the tick, and the mean |applied − asked| per tick, which
is the number that says whether the tick agreed with the picture.

**Measured**, with a synthetic mouse (a local test hack, not committed) turning at
a constant rate in bursts of 430 ms, at 144 fps against the 20 Hz world, slot 2:

| | lead off | lead on |
|---|---|---|
| view starts moving after the hand | 26.3 ms (max 53) | 4.1 ms (max 7) |
| view stops after the hand | 71.2 ms (max 94) | 4.9 ms (max 23) |
| steady turn, view against the game's own angle | 24.9 ms behind | 21.5 ms ahead |
| steady turn, speed sd at tick frames / between | 0.13 / 0.15 | 0.18 / 0.19 |

The lead is under a frame at 144 fps, and a steady turn is as even across a tick
boundary as between them. The one visible wart: when the hand stops, the view
settles back by **1 unit** (0.09°) at the next tick on about one stop in ten,
because the game truncates its step and carries the fraction while the picture
rounds. Pacing held at 144.0 fps drawn and 20.0 ticks/s.

**Not judged by eye**: whether it feels right on a real mouse, and on a
high-refresh monitor in particular. Also not measured: a mouse turn made while
opening a menu shows for up to three ticks and is then dropped, since the look
routine never spends it.

## The menu pointer

Mouse look leaves the menus pad-only: Circle opens `func_80018E80`, and from
there every list is walked with Up/Down and confirmed with Cross. A hand on the
mouse has to go back to the keyboard to use an inventory.
`patches/MenuMouse.cs` closes that — point at an item and the game's **own**
cursor moves to it, left click confirms, right click backs out, and a left click
off to the side of the menu backs out too — and it is on by default, because
unlike mouse look it needs no captured pointer: a player who never locks the
pointer still has one.

### There are three menus, not one

The first version of this patch shipped knowing only the first of them, and was
reported from play as *working on the main tab menu and doing nothing in any
submenu*. That is exactly right and it is not a bug in the mechanism: the game
builds a menu out of three unrelated widgets, and the patch implemented one.

| widget | drawn by | stepped by | the cursor is | pages |
|---|---|---|---|---|
| fixed option list | `func_800208D8` | `func_8001EA14` | the caller's register, returned in `V0` | 7 |
| scrolling list | `func_800209E0` | `func_8001EB70` | `u8[desc+0x21]`, in memory | 16 |
| two-line prompt | `func_80021478` | inline in `func_800206E0` | that function's `S1`, and nowhere else | 15 call sites |

So there are three mechanisms in the patch rather than one generalisation,
because the three cursors are not the same kind of thing. What they share is the
pointer, the hit test, and the "whichever device moved last owns the cursor"
rule. See "The scrolling list is one descriptor" and "The two-line prompt keeps
its cursor nowhere" in [GAME_INTERNALS.md](GAME_INTERNALS.md) for the widgets
themselves.

### The fixed list is met at its return

**Injecting Up/Down does not work, and that was already written down.** See "The
wall is the title, not the Continue menu" in
[PATCHES_AND_MODS.md](PATCHES_AND_MODS.md): driving `PAD_dr` at the start menu
moved `func_8001EA14`'s cursor not at all, while Cross registered every time.
And the second-guess route is worse — the menu does not read stage 3's pad word
at `0x80199554`; `func_80022E58` calls `PadRead(1)` itself.

So this drives the stepper's **return value**. The stepper is
`func_8001EA14(cursor, maxIndex, *selected, *confirmed, *cancelled) -> cursor`, a
post-hook writes `V0`, and the whole of the pad is left alone.

That also answers the question a synthetic Cross would have raised. **Neither
stepper edge-detects** — that is the finding `patches/MenuPacing.cs` exists for —
so a held synthetic confirm fires on every iteration of the menu loop and runs
away through the submenus. A click is edge-detected in the patch and spent once,
by writing the stepper's own out-parameters exactly as its Cross arm does:

```c
*confirmed = 1;
if (cursor < maxIndex) *selected = cursor; else *cancelled = -1;
```

including the arm's own quirk that **confirming the last entry is a cancel**.

Its geometry is the one thing here that cannot come from the stepper's own
arguments: it is read at the *drawer*, `func_800208D8`, out of the static layout
table, and cached with a 500 ms staleness bound so a page change cannot leave a
list on file that is no longer on screen. The other two need no such pairing.

### The scrolling list has a cursor in memory

`func_8001EB70(desc, items, *confirmed, *cancelled)` returns the **pad word**, not
the cursor: the cursor is `u8[desc+0x21]` and its row on the page is
`u8[desc+0x22]`, both stepped in place. So hover writes those two bytes and is
done. `u8[desc+0x20]`, the page, is what *hover* never writes — a pointer can only
reach a row that is on screen, so the three stay consistent by construction — and
that is the byte the wheel took, two sections below.

A move also replays the stepper's own move arm, which is not only the blip:
`func_80022CAC(items[cursor])` is what loads the entry's preview. Skip it and the
list moves while the picture beside it does not.

The geometry comes off the same descriptor, live — row `r` is
`(u8[+0x1C], u8[+0x1D] + 5 + 14*r)` at 236 × 14 — so there is nothing to cache and
nothing to pair, and a page this patch has never heard of is measured correctly
the first time it is drawn.

### The prompt is the one place that goes through the pad

`func_800206E0` keeps its yes/no cursor in `S1` for the length of its own modal
loop. There is no return value to meet and no byte to write, so this is the one
widget that has to press a button — and it can do that safely only because the
loop *tells the patch its state every iteration*: `func_80021478` is handed the
flag as its third argument.

A post-hook on `func_80022E58` — the loop's own `PadRead(1)` — ORs one synthetic
Up into the returned word while the hovered box disagrees with the drawn flag, and
one Cross once they agree. **The injection is closed over the state it changes**,
which is what the no-edge-detection finding actually forbids: the next iteration
reads the flag its own toggle produced and stops asking, so a held pointer cannot
run away the way a held synthetic Cross would. A left click is left *pending*
while the flag is still wrong, so pointing at the far box and clicking is one
gesture that takes two iterations rather than a click that misses.

It is scoped to `func_800206E0` being on the stack, so no other menu — and nothing
outside a menu — ever sees an injected button. The latch at `0x8006E5C4` is
`func_80022E58`'s own and is set from the *real* pad word before the post runs, so
an injected button costs no repeat delay, exactly as a hover elsewhere costs none.

### Backing out is the pad read's, not each widget's

**Reported from play: "the back out behaviour in menus is pretty inconsistent —
sometimes right click works, other times you need to press Tab to get out."**
That is exactly what the patch did, and the reason is countable. Backing out was
implemented three times, once inside each widget's own hook, writing that
widget's `*cancelled` out-parameter. But **six** routines read the pad inside a
menu:

| routine | what it is | has a cancel arm |
|---|---|---|
| `func_8001EA14` | the fixed list's stepper | yes |
| `func_8001EB70` | the scrolling list's stepper | yes |
| `func_800206E0` | the two-line prompt's loop | yes |
| `func_8001BB7C` | draws a fixed list, reads the pad itself | yes |
| `func_8001BE60` | draws a fixed list, reads the pad itself | **no** |
| `func_8001B0D0` | reads the pad itself | no masks of its own |

The patch covered the first three. So right click backed out of the tab menu, the
inventory and a yes/no prompt, and did nothing at all on the two screens that
draw a fixed list and then read the pad themselves — the fourth shape this
document already recorded as uncovered. Two of six was the inconsistency.

**All six call `func_80022E58`**, the menu's `PadRead(1)`. So the gesture no
longer writes anything: it raises one flag, and a post-hook on that read ORs the
game's own cancel mask (`0x8006E56C`, the first of the two every arm above tests)
into the word it returns. Whichever routine is reading gets it, its own cancel arm
runs, and the blip and the out-parameter are the game's rather than a copy of
them. A screen with no cancel arm ignores it — which is the honest answer, because
that is what the pad's cancel button does there too. `func_8001BE60` is the one
such screen found so far.

**It cannot run away**, which is the thing the no-edge-detection finding forbids.
The flag is spent on the read that delivers it and one menu-loop iteration is one
pad read, so a click backs out exactly one level; a held button is not a held
injection.

**Two gestures raise it.** Right click anywhere over the picture — including the
widescreen margin, which is part of the presented picture and is about as far
"off to the side of the menu" as a pointer can get. And left click **clear of the
widget's own boxes**, by 8 game pixels on every side. Clear of them rather than
merely off a row: the fixed list's boxes are 26 apart and 24 tall, so testing
`!hit` would turn the 2px gutter between two of them into a back-out and a sweep
down the list would close the menu on the way past. The three answers are on a
box (confirm), near the boxes (nothing), clear of them (back out).

**The bounds are the union of the boxes the widget drew and nothing more.** The
frame around them, the item picture beside a list and the description under it are
drawn somewhere this patch does not know, so a click on those reads as a click off
the menu. Whether that is felt as wrong is a question for the eye, not for a
counter.

**The button edges are sampled at the pad read too**, not only in the widget
hooks, and that is a second half of the same defect: a press *and* release that
both happened while no hooked stepper ran used to be seen by nothing, so a click
made on an uncovered screen was simply lost. The same hook carries the session
scope for those edges — a gap of 500 ms in `func_80022E58` calls means the menu
that was reading the pad is not the one reading it now, so the buttons are re-read
rather than left to fire an edge, and a back-out asked of a screen that has gone
is dropped. A menu-loop iteration is 105–166 ms here (`func_80022E90`'s six held
vblanks are 100 ms of it), so 500 ms is three of the longest of them. That scope
covers the menus **outside** `func_80018E80` as well, which `BeforeMenu` never
could: the save-slot list off the object-use handler, a shop off an NPC.

### The hit test is the rectangle the game drew

Not a band, and not one axis. Each of the three widgets draws its rows as quads
whose corners the patch computes from the numbers the drawing routine itself uses:

- **fixed list** — `func_800218B4(template, record)`, so `(X-6, Y-6)` by `w` × `h`
  with `w`/`h` from the template at `0x80064C20` (124 × 24).
- **scrolling list** — `func_800209E0`'s own layout, 236 × 14 packed at 14.
- **prompt** — `func_800218B4` again, the 54 × 24 template at `0x80064C08`, the
  two records taken from the drawer's arguments rather than assumed.

The version this replaces synthesised a band per row instead — the smallest gap
between two rows, floored at the box height, tested against Y alone — and it was
wrong in three ways at once. It was **six pixels low**, because the record's `Y`
is the sprite's position and the box is drawn six up and left of it (the note in
[GAME_INTERNALS.md](GAME_INTERNALS.md) had that inset on the *size* instead of on
the origin, and has been corrected). It ran each row's band into the gutter, so a
click between two boxes selected the one above. And testing Y alone made the whole
width of the screen live, so a click far to the right of a 124-pixel box confirmed
it.

**X is exact rather than avoided.** The old reasoning was that widescreen moves X
and Y is safe — true of the *conversion* and not of the test. The presented
picture is `GameW + 2*margin` game pixels wide with the game's own column 0 at
`margin`, `GlCore.PresentDisplay` builds it that way, and `Display.WideMargin` is
that number. Subtract it after the scale and a game X is a game X at every aspect;
measured at 16:9, `of 320x240 +54`.

The gutters are now dead rather than assigned to a neighbour: 2 pixels in 26 for
the fixed list and the prompt, and none at all in a scrolling list. A click in one
is a click on nothing.

### Where the pointer comes from

The pointer's own position comes from **ImGui's IO** rather than from a new host
API. It is in the same screen space as `OutputView`, it is a plain field read so
it costs nothing per call, and `patches/MapPanel.cs` already reads it. It carries
the last presented frame's value, which is the right one: every menu loop presents
through `func_800226A8`, and the panels draw inside that `VSync`.

Turning a window pixel back into a game pixel needed one thing the runtime did
not publish — the picture's size in the game's *own* pixels. `OutputView` had
`Min`/`Max` and no scale, and `Display.LastDisplayW/H` is not re-exported, so
`patches/recompone/0029` grew `GameW`/`GameH`, set from the `SetTexture` call
that already receives both. UI only, no recompile. `GameW` is deliberately the
game's own width and not the presented one, which is why the widescreen margin is
added back in the patch rather than read off the picture.

**`OutputView` is read directly here and `MapRender.Picture` deliberately is
not.** That helper falls back to the whole viewport when the panel drew no
picture, which is right for something that has to be drawn *somewhere* and wrong
for a coordinate conversion: a hit test would rather answer "nowhere" than answer
confidently against a rectangle the game is not in.

### Whichever device moved last owns the cursor

Hover takes the cursor only once the pointer has **moved**, and hands it back the
instant the pad or the keyboard moves it. Without that, a mouse resting anywhere
over the picture would pin the selection to whatever row it happens to sit on and
the D-pad would appear broken. A movement stays live for two seconds.

In the prompt that hand-back needs one extra thing, since there is no cursor to
compare: the drawn flag changing to something the patch did **not** ask for is the
pad, and takes the cursor back.

Opening the menu also **gives the pointer back** if mouse look had it: under
`CursorMode.Raw` GLFW reports an unbounded virtual position, so there is no "over
the picture" while it is locked, and no visible cursor to point with either. It
is retaken on the way out. Both ends go through `Mouse.SetCaptured`, so the glyph
in `patches/MouseIndicator.cs` announces them, and the `PadReadEvent` button
listener detaches with the capture — which is what keeps a menu click from also
arriving as a Square.

### The wheel owns the page, not the cursor

**A list longer than its window was unreachable with the mouse.** Pointing gets
you any row that is *drawn*, and the pad's Down is the only thing that turns the
page — so with an inventory of seven entries in a four-row window, three of them
could not be selected without going back to the keyboard. Measured, off
`KF2_MENUMOUSE_PROBE=1` in area 1: `list at 0x801FF8C8, 7 entries, 4 visible from
0`. That is the whole of the report from play that this section answers.

The first version of this patch had a wheel and took it out, and the reason was
good: it stepped the **cursor**, relative to where the cursor was, while hover
puts the cursor where the pointer is. Two rules for one byte, contradicting each
other on the very next iteration of the menu loop — scroll two rows and hover
snaps it straight back to whatever the pointer is over.

The way out is that the scrolling list has a **second axis**, and it is the one
byte hover has never written. `+0x20` is the page and `+0x21` is the selection.
Give the wheel the page and leave hover the cursor and there is nothing to
contest: scrolling under a still pointer changes which entries the rows *show*,
hover then reads off the row the pointer is on, and both orders give the same
answer. The invariant `+0x22 = +0x21 − +0x20` is what makes it exact — a page
that moves under a cursor that did not still rewrites both bytes, or the
highlight is drawn on the wrong row.

**Clamped, not wrapped.** The page may reach `count − visible` and no further,
which is where `func_8001EB70`'s own wrap-to-the-bottom arm puts it; but the
wheel deliberately does not wrap, because it is a continuous gesture and a list
that jumped to the far end when it ran out would be unusable. The pad's Down
still wraps — the game's stepper is untouched.

**It is the scrolling list's alone**, and that asymmetry is the finding rather
than a gap. A fixed list and a prompt draw every row they have, so there is no
page to move; on those two the cursor *is* the only axis and a wheel would be
straight back to fighting the pointer for it. A notch spent over one of them is
therefore discarded rather than saved, or it would fire the moment a scrolling
list opened — a list that pages itself on the frame it appears.

### The notch is taken from the host, not listened for

`patches/recompone/0038` gives `HostWindow` a `TakeMouseWheel()` beside the
`TakeMouseMotion()` that `0017` added, and it is the same shape for the same
reason: the host produces scroll as **discrete events**, so an accumulator that
is drained cannot miss one or spend one twice, where a caller reading a level
would do both. ImGui's `io.MouseWheel` is that level — it is refilled each
`NewFrame`, and a menu loop iterates at 30 a second against a 144 fps window, so
a notch would be lost whenever two frames passed between steps and repeated
whenever none did.

Two more things follow from taking it off the host rather than off a
`MouseEvent` listener. **Not every scrolling list is inside `func_80018E80`** —
the save-slot menu is opened from the object-use handler and a shop from an NPC —
so a listener scoped to the menu session would have covered the inventory and
quietly missed both. And nothing in the runtime listens for `MouseEvent` at all,
so registering one would have started dispatching an allocation for every pointer
*move*, hundreds a second, all through play.

The wheel is a **float** end to end. `OnScroll` had `Wheel = (int)wheel.Y`, which
is exact for a discrete wheel — GLFW steps that by ±1 — and rounds a trackpad's
two-finger scroll, which arrives in fractions of a notch, away to nothing. The
patch keeps the remainder and spends whole notches, one notch to one row: a notch
is the gesture's own unit and the pad's Down at the window's edge pages by
exactly one, so a wheel that moved faster than the D-pad can would be a different
control rather than the same one on another device.

### What it writes

For the fixed list, `V0`, the stepper's own out-parameters, and `0x8006E5D0` — the
blink direction, zeroed to restart the wink exactly where the Up/Down arms zero
it. For the scrolling list, `u8[desc+0x21]` and `u8[desc+0x22]`, `u8[desc+0x20]`
when the wheel turns, plus that stepper's own `*confirmed`; it does
*not* write `0x8006E5D0`, because `func_8001EB70` does not either. For the prompt, nothing at all in game memory —
only one bit ORed into a register on the way out of `PadRead`. **No `*cancelled`
is written by any of them any more**: backing out is one bit ORed into that same
register, and the game's own arm writes the out-parameter.

`0x8006E5C4`, the repeat gate's latch, is untouched by all three, so `MenuPacing`
is unaffected in both directions: a hover costs no repeat delay because it never
goes through the pad, and the pad's own repeat is exactly what it was.

### What is measured, and what is not

Measured at `KF2_FPS=144` in area 1, off `KF2_MENUMOUSE_PROBE=1`:

- All six hook groups attach and are read back from `HookManager`, not from the
  `Add*` returns: `session scoped, tab menu driven, lists driven, prompt driven,
  back out driven`.
- The fixed table reads correctly on two groups — group 0 at `0x80064CD4`, eight
  boxes at x 25, y 13/39/65/91/117/143/169/195, 124 × 24; group 6 at `0x8006540C`,
  two at x 92, y 88/114, the same size. That the second lands where
  `base + 6 * 0x134` says it does is the check on the stride.
- The scrolling descriptor reads correctly on two lists on the same page — 10
  entries, 10 visible from 0, rows at x 42 y 44, 236 × 14; and 3 entries, 4
  visible, rows at x 42 y 164.
- A list **longer than its window** exists and is the reported defect: 7 entries
  in a 4-row window at x 42 y 164, three of them unreachable by pointing. The
  probe's per-second line names the page for it now — `list, rows 0-3 of 7`.
- The pointer converts: at 16:9 the probe reads `of 320x240 +54`, and a desktop
  pointer sweeping the inventory answers rows 0, 1, 2, 4, 5 and 6 in order, with
  −1 above the first row of the short list.
- Hover, confirm and cancel all fire from a real hand on a real mouse, and the
  session ran with no exception. Backing out fires through the pad read from
  both gestures and from both sides of the picture — `cancelled 1, injected 1`
  at game x −14.4 and again at x 346.9, the two widescreen margins, with the tab
  menu's eight boxes drawn between them.
- It costs nothing: 20.0 ticks/s with the menu open, and the menu's own 60 fps
  under `LoopPacing` is unchanged.

**The wheel's own gesture is not measured, and cannot be from here.** The shell's
`press` verb reaches Circle and Cross but not the menu's Up/Down — that is the
"injecting Up/Down does not work" finding above — and there is no way at all to
inject a scroll, so the arithmetic below `TakeWheel` has been read rather than
run. What *is* established from the game's own code is the two arms it copies:
`func_8001EB70`'s Down steps `+0x22` while the cursor is inside the window and
`+0x20` only at its last row, and its Up mirrors that at row 0 — so `+0x20` moves
by one at a time, is bounded by `count − visible`, and `+0x22` is `+0x21 − +0x20`
at every step of both.

**Not measured, and it needs a person.** Whether one notch a row is the right
speed. Whether the cursor lands on the item the pointer is *actually* over — the numbers can only say it lands on the row the
descriptor or the table says is there. Whether a desktop arrow over a 1996 menu
reads acceptably. Whether a move blip on every row crossed is pleasant or noisy.
Whether the 2-pixel dead gutters are felt when sweeping down a list. And the
prompt's injection path has been reasoned about rather than watched: no run so
far has opened a yes/no prompt with the pointer over it.

**Still not covered.** `func_8001BB7C` and `func_8001BE60` draw a fixed list with
`func_800208D8` and then read the pad themselves rather than calling
`func_8001EA14`, so they are a fourth shape, with a fourth place the cursor could
be living. Neither has been identified by what it is on screen.
