# Patches and mods: how the port's own code is attached

Two ways to run C# against the recompiled game, and a rule for choosing between
them. Then the settings UI both feed into, and the two features whose whole story
lives here — frame pacing and auto reload.

## Where each patch is written up

| file | what it does | detail |
|---|---|---|
| `FramePacing.cs` | paces the picture, and holds the world to 20 Hz | this file |
| `MenuPacing.cs`, `LoopPacing.cs` | run a loop that renders its own frames at the world's rate, and fill the gap by redrawing | this file |
| `LoadPacing.cs` | makes a `VSync(0)` inside a disc wait cost a vblank again, so the loading screen's figure walks at the console's rate | this file |
| `MenuWorld.cs` | draws the world live behind a menu or a shop instead of the frozen 320-wide copy | this file |
| `MessageText.cs`, `MessageGlyphs.cs` | draws sign and dialogue text as text on an opaque box, decoded from the message's own picture (experimental, off) | this file |
| `SpriteAnim.cs` | steps the billboard sprites' cels once a world tick, so the flames burn at the console's speed | this file |
| `FrameSmoothing.cs`, `ObjectSmoothing.cs`, `AnimSmoothing.cs`, `FluidSmoothing.cs` | carry the view, everything that moves, MO clip time, and the scrolling textures, between ticks | this file |
| `DrawCensus.cs` | attributes the frame's primitives to the routine that drew them | [GAME_INTERNALS.md](GAME_INTERNALS.md) |
| `PolyAssembler.cs`, `PolyAssemblerLit.cs` | `func_80030540`, the polygon assembler, `func_8002FECC`/`func_8002E650`, the far map tiles' assembler and its vertex transform, and `func_8002F214`/`func_8002EAEC`, the models' lit assembler, rewritten in C# as replace hooks | this file |
| `AutoReload.cs` | reloads the last save on death | this file |
| `Map.cs`, `MapMarkers.cs`, `MapRender.cs`, `MapPanel.cs`, `MapOverlay.cs`, `MapFog.cs` | the area's floor plan, what is standing in it, and the tiles you have seen | this file |
| `NoDither.cs` | clears the GPU dither bit | [RENDERING.md](RENDERING.md) |
| `Perspective.cs`, `Subpixel.cs`, `ZBuffer.cs` | switches and probes over the GTE depth mechanisms | [RENDERING.md](RENDERING.md) |
| `Widescreen.cs`, `CullCone.cs`, `ViewClip.cs`, `PrimBuffer.cs` | aspect ratio and the culls it runs into | [WIDESCREEN.md](WIDESCREEN.md) |
| `Analog.cs`, `AnalogProbe.cs`, `Mouse.cs`, `KeyLayout.cs` | pad, mouse and keyboard | [INPUT.md](INPUT.md) |
| `settings/InputSection.cs`, `settings/BindingTable.cs` | the Input pane, which the port draws itself | [INPUT.md](INPUT.md) |
| `EndingHold.cs` | keeps the window alive through `END.EXE`'s final spin | [RUNTIME.md](RUNTIME.md) |
| `UiScale.cs` | forces and saves the interface scale, for a config too large to edit in | [RUNTIME.md](RUNTIME.md) |
| `settings/*` | the pages all of the above draw into | this file |

`mods/kf2debug` is the measurement/debug tool that stayed a mod; its findings are
in [GAME_INTERNALS.md](GAME_INTERNALS.md).

## Mods

**RecompOne has a modding system; use it.** `RecompOne.Runtime/Modding/` is 839
lines of `ModLoader`, `HookManager`, `SymbolRegistry`, hook attributes and a
`ModsPopup` UI, and the generated `Entry.cs` already calls `ModLoader.LoadAll()`.
It was missed on the first pass here and a parallel one was built by hand; that
was wasted work and two wrong beliefs, both recorded below so they are not
re-derived.

A mod is a folder (or zip) under `mods/` with a `mod.json` and C# sources,
**compiled at run time by Roslyn** and hooked by address:

```
mods/kf2debug/mod.json   + GameState.cs,     noclip, invincibility, warp, the
                           Noclip.cs,        character and inventory editors,
                           Cheats.cs, Warp.cs,   and a live state readout
                           Attributes.cs, Items.cs,
                           Hotkeys.cs, DebugPanel.cs, DebugMod.cs
```

(What building `kf2debug` turned up about the game is under "Debug tools" in
[GAME_INTERNALS.md](GAME_INTERNALS.md).)

Two things the debug mod needed that are not obvious from the mods above:
**`PanelManager.Register(IPanel)` is public**, so a mod can add a dockable window
next to CPU State rather than living inside the Mods popup — but panels do *not*
auto-populate the menu bar (`MainMenuBar` declares each built-in one by hand), so
it also needs a `MenuRegistry.Menu(...).Panel<T>(...)` entry. And
**`HostWindow.IsKeyDown(Key)` is public**, which is the route to real hotkeys;
note it lives in `RecompOne.Runtime.Host`, not `.Host.Window` like everything
else in that folder. F1 and F11 are the host's own.

```csharp
[PostHook("game", Address = 0x80060818)]
static void AfterDrawOTag(CpuContext c, IMemory m) { ... }

[PreHook("game", Address = 0x80037C0C)]        // return false to skip the original
static bool BeforeStage(CpuContext c, IMemory m) => ...;
```

`HookManager` detours the emitted method through MonoMod, so **a hook site costs
no config entry and no recompile** — `SymbolRegistry` resolves `overlay + address`
(or `overlay + function name`) against the dispatcher's tables, which are all
registered up front in `Entry.Run`. `[Replace]` also exists, and can take a
leading `orig` parameter to call through to the original.

Mods are toggled in the game's own Mods panel, with reload, and `IMod.DrawSettings`
puts an ImGui panel under each one. State persists to `interface.ini` as
`mods.<id>.enabled`.

### What belongs in a mod, and what does not

`patches/FramePacing.cs` is deliberately **not** a mod. It is a correctness fix
that has to be on: as a loaded package it could be absent, disabled, or fail to
compile, and the failure mode would just be the game running too fast. It still
attaches through `HookManager` — the same runtime detour, no config entry — just
from `Program.cs` with its own `ModInfo`, so it cannot be turned off by accident.
Measurement tools, which are genuinely optional and expensive, are real mods.

`patches/NoDither.cs` (see "Dithering" in [RENDERING.md](RENDERING.md)) moved the
other way — it was `mods/nodither` — and the
reason is not correctness. It is that **a mod is a package that can be absent, and
a picture the port offers should not be**: the dither switch is a graphics option
like vsync, it costs nothing when off, and a player looking for it in Display
should find it there rather than have to know a package exists. That is the
general test for the move. Being a patch is also what lets it default to *on*
(dither cleared) — a mod defaults to disabled and would have shipped doing
nothing. The conversion itself was mechanical: `[PreHook]`/`[PostHook]` attributes
became `SymbolRegistry.Resolve` plus `HookManager.AddPre/AddPost` in an `Attach`
deferred to the first `OverlayLoadedEvent`, hook bodies had to become `public`,
`OnLoad`'s config read moved to `RuntimeReadyEvent`, and `DrawSettings` became an
`IPatchPage`. Nothing about the hooking changed, because `HookManager` is the same
detour either way.

`patches/AutoReload.cs` moved for a third reason, which is the same test applied
to behaviour rather than to a picture: **four screens of menu after every death is
not a taste a player should have to find a package to fix.** It is the first patch
whose settings are not a machine option at all, and it is why the port has a
Gameplay section — see "Auto reload became a patch" below.

`patches/Analog.cs` is the same test applied to the *pad*, and it is the clearest
case of the four: with the patch absent, a modern controller's left stick is bound
to the D-pad, and the D-pad in this game turns rather than walks — so the port's
answer to "I plugged a controller in" is a stick that spins the camera. That is
not a taste either. It defaults to on for the reason under "Analog control became
a patch" in [INPUT.md](INPUT.md): **sticks centred means the hooks return before
touching memory**, so
keyboard and D-pad play with it on is identical to play with it off, and the cost
of shipping it enabled is nothing at all.

`patches/Widescreen.cs` is the dither test again — an aspect ratio is a picture
the port should offer without a package having to load — and it is the case that
separates that test from the *default*. Being a patch decides where the switch
lives; it does not decide which way the switch points, and this one ships pointing
at 4:3 because the picture has never been checked by eye. See "Widescreen became a
patch" in [WIDESCREEN.md](WIDESCREEN.md).

Config `patches[]` entries are still the right tool for **`replace`**, which is
how the whole SDK is bound: those are needed before any mod could load, and there
are 63 of them.

### Four things that will bite

1. **`ModCompiler` does not enable implicit usings.** Every mod file must name
   `System`, `System.Collections.Generic`, `System.Linq` itself. This is the real
   cost of runtime compilation: it is a *runtime* error, printed as
   `[Mods] <id>: ... error CS0103` and then the mod silently does not load.
2. **`mods/**` must be removed from the csproj `Compile` glob**, exactly like
   `tools/**`. Otherwise the SDK compiles every mod into the main assembly *as
   well as* Roslyn compiling it at run time, giving two copies that can drift.
3. **Mods default to disabled** (`IsEnabled` falls back to `false`), and
   `LoadAll` returns silently when nothing is enabled — no log line at all. A mod
   that appears to do nothing is probably just off.
4. **`mods/.cache/`** holds the compiled assemblies and is written into the repo;
   it is gitignored.

**Seen once, not reproduced, and not a fifth item:** the very first run with the
dither mod hung after both mods reported their hooks and before
`ModLoader.LoadAll` printed `loaded N/N` — i.e. in `HookManager.Commit`, with the
worker thread absent from `dotnet-stack` output entirely. Five later runs of the
same pair committed instantly. Recorded so it is recognised rather than chased if
it ever happens again: the stack will show the main thread in
`ModLoader.LoadAll → HostWindow.Pump`.

## Patch settings: a patch's knobs go in the runtime's own sections

A mod gets a settings UI for free — `IMod.DrawSettings` is drawn under the gear
button in the Mods popup — and a patch got nothing, so a patch's only knob was an
environment variable read once at startup. That is the real cost of moving
something out of `mods/` and into `patches/`: the code keeps working and the whole
panel disappears. `patches/settings/` is the replacement.

**It is not a panel of its own.** `SettingsRegistry.Extend(sectionId, draw)` takes
a section id and a callback and runs it after that section's own content, which
means a patch's settings can go where a user would already look for them. The
frame rate belongs under Video beside vsync and render scale; a King's Field
box off to one side would be a worse place to keep it. So a patch registers a page
against a section:

```csharp
PatchSettings.Register("display", new FramePacingPage());   // IPatchPage: Id, Title, Draw()
```

The section ids are the runtime's own — `interface`, `input`, `display`, `paths`,
`audio` — plus `gameplay`, the one section the port registers itself, for patches
that change how the game plays rather than how the machine behaves (see "Auto
reload became a patch"). Registration is deferred to `RuntimeReadyEvent`, which is
late enough that `HostWindow`'s `Load` has registered the runtime's five — so the
port's own section can join them there, and an id matching no section is reported
at startup instead of silently drawing nothing. A page is plain ImGui,
which is what makes this the landing site for a converted mod: its `DrawSettings`
body moves across unchanged. `NoDitherPage` is the first one that arrived that
way — `mods/nodither`'s checkbox, in Video under the frame rate, with the mod's
`_on` field now `NoDither.Enabled` and its `Runtime.View` calls now
`PatchSettings.Set`. (That page no longer exists as a file: it is one entry of
`ShadingPage`'s slider now — see "Two shading checkboxes were one question asked
twice" — but the conversion is what the paragraph is about and is unchanged.) Its explanatory paragraphs did *not* come across: a mod's
panel is a place to explain itself, a settings section is a list of switches, so
the prose became a hover tooltip and the counters stayed on the console.

**Pages sharing a `Title` share one heading.** `SeparatorText` over a lone
checkbox is the checkbox's own label written twice with a rule through it, so the
shading control is titled `Enhancements` and anything else of that size can join
it there — perspective correction, sub-pixel positioning and the Z-buffer all
did; `FramePacingPage`, which is a slider plus a live measurement, keeps its own.
Drawing a heading only when the title changes is the whole implementation.

**The title is the heading and `Order` is the sort key, and that used to be one
field doing both jobs.** The list was sorted by `Title` alone, which meant the
order of the port's two groups in Video was an accident of how they were spelled:
`Enhancements` drew above `Frame pacing` because `E` sorts before `F`, and
nothing said so. The order *within* a group was worse — five pages with equal
keys, held in registration order only because `List<T>.Sort` happens to use an
insertion sort below sixteen elements, which is an implementation detail and not
a documented guarantee. So `IPatchPage` has an `int Order => 0`, defaulted, sorted
ahead of the title. The eight pages outside Video state nothing and are unchanged
(their titles are all distinct, so none of them depended on the tie-break); the
six in Video number themselves 10, 11, 20, 21, 22. The one constraint the
heading logic imposes is that **pages sharing a title need adjacent orders**, or
a page between two of them draws the heading twice.

**The order it produces is the argument.** Frame pacing goes above Enhancements
because the frame rate is the option a player came to Video for and the fidelity
switches are the port's extras, and *Smooth motion between game ticks* moved out
of Enhancements to sit directly under the frame-rate slider — it is greyed out
whenever the rate is not above the world's tick, so at the shipped 60 fps it is
live, and the control that decides that is the one immediately above it. A dead tick met
before its cause reads as a bug; met under its cause it reads as a note.

**A page could not get *inside* a runtime section, and now it can.** `Extend`
draws after the section's whole body, so an option that is one of the section's
*ordinary* options — an aspect ratio, which is the same kind of choice as the
render scale — could only land in a block underneath everything, below the GPU
backend combo, which reads as the port's box rather than as a picture setting.
`patches/recompone/0013` is the fix and it is six lines:
`SettingsRegistry.DrawSlot(slotId)` walks the same `Extend` table by an arbitrary
id, and `DisplaySettingsSection` calls it once, right after the render scale, as
`"display.render_scale"`. `PatchSettings.RegisterSlot` is the port-side half; a
slot page draws **bare**, with no `SeparatorText`, so it sits in line with
fullscreen, vsync and render scale rather than under a heading.

The asymmetry to know about: `Register` reports an unknown *section* id at
startup, because the sections are enumerable. **An unknown slot id is silent** —
`Extend` takes any string and nothing lists the slots — so a typo there costs a
control that simply never appears.

**A page big enough to need shaping is the point where two rules stop scaling.**
`AnalogPage` is eighteen knobs in the `input` section, under the button-binding
table, and it broke both of the small pages' habits:

- *Static properties with `Set…` methods* are how the other patches expose live
  state, and they exist to clamp. Eighteen of them would clamp nothing a slider
  had not already, so `Analog`'s settings are plain **public static fields** —
  which is also what lets the page pass them straight to `ImGui.SliderFloat` by
  `ref` instead of copying a value out and back. The clamp that a slider does
  *not* do is ctrl+click typing, and `ImGuiSliderFlags.AlwaysClamp` is that.
- *A flat list of switches* is right for three of them and wrong for eighteen. The
  three that matter — the master switch and the two sensitivities — stay at the
  top, and the deadzones, curves, acceleration ramp, axis inversions and the probe
  go under an `ImGui.TreeNode`, with `BeginDisabled` greying the lot when the
  master switch is off.

It also carries the first thing in a patch page that is **not** a setting: a live
readout of both sticks, drawn dimmed at the bottom. Deadzone is the setting people
get wrong, and the number that decides it is how far the pad in hand rests from
centre — without the readout that is a guess, checked by walking into a wall.

### Renaming a runtime section without patching the checkout

"Display" is where the window lives; everything in that section is how the picture
is made, and the port only adds to it — a frame rate and a dither switch beside
vsync and render scale. Renaming it to **Video** turns out not to need
`patches/recompone/` at all: `SettingsPopup` draws each tab from
`Localization.T(section.TitleKey)`, and `Localization.Merge(json)` is public and
overwrites by key, so `PatchSettings` merges one string for `settings.display` at
the same `RuntimeReadyEvent` it registers the pages.

**Only English is overridden**, because the runtime's other two languages already
say exactly this: pt-BR is `Vídeo` and es-419 is `Video`. Overriding a key means
supplying every language that key has, or the ones left out keep the old word —
here the old word is already the new one.

The **id stays `display`**. Only the label moves, so `Register("display", …)`,
`SettingsRegistry.Extend` and the runtime's own section are all untouched. Two
things make this safe: `Merge` calls `EnsureBase` first, so the embedded table is
loaded before the override lands on top of it rather than being loaded over it
afterwards; and the override goes into the per-language tables themselves, so a
later `SetLanguage` re-points at a table that already carries it. Verified against
`RecompOne.Runtime.dll` directly — the sibling keys (`settings.display.vsync`,
`settings.input`) are untouched, and `settings.display` has exactly one consumer
in the runtime, that section's own `TitleKey`.

This is worth knowing generally: **anything the runtime shows through
`Localization.T` can be renamed from the port**, which is a much cheaper lever
than a patch to a gitignored checkout.

**A heading over the runtime's own controls was tried and dropped.**
`SettingsPopup` draws one `SeparatorText` per pane, from the section's
`TitleKey`, and then the section's content bare, so fullscreen and vsync end up
the only unlabelled controls on a page whose other groups are named. Wrapping the
section works — `ISettingsSection` is public, `SettingsRegistry.Register` replaces
by id, and a wrapper forwarding `Id`/`TitleKey`/`Order` can draw a heading before
delegating `Draw` — but it buys a rule and a word for four controls the pane title
already names, and it is one more thing to keep working against an upstream that
does not know about it. The pane is `Video` and the port's groups are named under
it. If the runtime ever grows more sections worth grouping, the missing per-group
heading is upstream's to fix and is worth an issue rather than a wrapper.

**That judgement was about Video and it did not survive Input**, which is the
next subsection: a wrapper buying a rule and a word for four controls a pane title
already names is a bad trade, and one buying a pane the port's own pages could not
otherwise be *seen* in is not the same trade at all.

### A section can be taken over as well as extended

`Extend` appends, and for Video and Gameplay that is right. For **Input** it was
not: the runtime's own pane is two tab bars, sixteen binding rows and a reset
button, about 450px of a 500px popup, so the port's four pages landed below the
fold — and the two keyboard-layout buttons *write* the sixteen rows of the table
a screen above them. So `patches/settings/InputSection.cs` **replaces** that
section rather than joining it. Four things are worth carrying:

- **`Register` replaces by id** — `RemoveAll(s => s.Id == section.Id)` then
  `Add` — so a wrapper needs nothing but the same id, and no patch to the
  checkout. Do **not** `Unregister` first: it states removal where the intent is
  substitution, and it hides the one failure that matters, which is upstream
  renaming the id and turning a replacement into a *second* tab. Warn on that
  directly, in the shape the existing section-id check uses.
- **`Extend` has no un-extend.** The list is append-only with no removal API, and
  `SettingsPopup` draws the section body and *then* the extensions — so a page
  still registered against a taken-over id draws a second time under the wrapper's
  own layout, outside whatever structure it built, permanently. The pages have to
  be **dropped from the registry**, not reordered in it, and `PatchSettings.Register`
  refuses that id now so a later patch cannot re-open the hole.
- **A page drawn by a wrapper is a body, not a page.** `Order` is inert — the
  wrapper writes its sequence out — and `Title` keeps only its degenerate half,
  that an empty title declines the heading. That is the same rule `MapPage` and
  `AutoReloadPage` already use, reached from the other direction.
- **What it costs is an upstream copy.** `InputSettingsSection` is `internal`, so
  the binding table is a copy rather than a call, and a change to it upstream is
  silent — the checkout is gitignored, so `git diff` will not show it.
  `BindingTable`'s doc comment names the file and the pin, so a pin bump has
  something to diff. Set against that, the wrapper is three files and one
  `Register` line: deleting the line restores the runtime's pane exactly.

The full write-up, including why the `controls` section that was going to be
built is dead — `settings.input` already reads "Controles" in pt-BR and es-419 —
is "The Input pane is the port's" in [INPUT.md](INPUT.md).

Settings persist through `PatchSettings.Get/Set`, which is `Runtime.View` plus an
immediate `SaveView` — the same `interface.ini` store the mods use, keyed
`kf2.<patch>.<name>`. **A persisted default cannot be read in `Configure`:**
`ConfigManager.Load()` runs inside `HostWindow.Initialize`, which is inside
`Runtime.Initialize`, which is *after* `Program.cs`, and `Load` replaces the whole
`ViewConfig` — so an early read sees an empty config and an early write is thrown
away. Read it on `RuntimeReadyEvent`, dispatched at the end of that same
`Initialize`. `FramePacing` does this and keeps the precedence the mods use:
`KF2_FPS` beats the saved value.

### Two shading checkboxes were one question asked twice

*Dither off* and *True color (24-bit)* were two ticks under Enhancements, and
they are the console's two treatments of the **same** 15-bit banding: the dither
hides it with a 4x4 crosshatch, true color removes it by keeping eight bits (see
"True color" in `docs/RENDERING.md`). Two ticks cross into four states carrying
three meanings, and the fourth — dither on *and* 24-bit — is a smooth gradient
with a crosshatch laid over it, which is nobody's answer to anything.

So `patches/settings/ShadingPage.cs` is one control, `Shading`, with the three
states that mean something. It was a combo; it is now a slider with fixed
positions, the shape of the frame-rate slider, as *Texture filtering* and *SSAO*
are:

| entry | `NoDither.Enabled` | `TrueColor.Enabled` |
|---|---|---|
| `Dither (original)` | `false` | `false` |
| `None` | `true` | `false` |
| `Smooth (24-bit)` | `true` | `true` |

Note the polarity: `NoDither.Enabled` true means the crosshatch is *off*. `Smooth`
is the port's shipped default (`kf2.nodither.on` and `kf2.truecolor.on` both
default to `true`).

Both patches keep their key and their environment variable — `KF2_NODITHER`,
`KF2_TRUECOLOR` — so merging the control strands no config and takes no
comparison off the console.

**Reading the state back follows `FrameSmoothingPage`'s shape, not
`WidescreenPage`'s.** An aspect matching no preset gets a `Custom` entry, because
an arbitrary ratio is a real thing to have asked for. The contradictory shading
state is not: it is only reachable from `KF2_NODITHER=0 KF2_TRUECOLOR=1`, and
giving it an entry would put the pointless combination back on the page. So true
color is read as the master — on means `Smooth` — and any click writes both
patches, harmonising the pair, exactly as the smoothing tick writes its four.

**Not done: a note when the backend cannot do 24 bits.** True color is GL-only
and the software rasterizer is always 15-bit, so a note under `Smooth (24-bit)`
saying so looked right — but the Graphics backend combo offers `auto`, `gl45`,
`gl33` and `gl21`, all four of them GL, and `GlBackendKind` has no software
entry. The note would be for a state the pane cannot produce.

### The rate became a setting, so every hook is installed whatever it is set to

Hooks attach at the first overlay load; the rate is chosen from the settings long
after, and cannot add one then. So `FramePacing` hooks **all** of it up front —
the frame gate, the three gated stages, the delta scaler — whatever `KF2_FPS`
said, and each hook decides per frame whether to do anything. At 30 every one of
them runs the original and the port behaves exactly as it did before any of this
existed; that is what makes 30 a safe default rather than a code path of its own.

Two consequences of the rate being live rather than fixed at startup: `Attach` does
not skip when pacing is off, since "uncapped" is a choice that can be taken back,
and `AfterDrawOTag` tests `Enabled` itself.

Two further shapes worth copying. The rate is a **double, not a vblank divisor** —
"arbitrary" was the point, and 144 is not 60/n — and the saved key changed with
it, from `kf2.framepacing.vblanks` to `kf2.framepacing.fps`. `SavedRate` reads the
old key once, converts (`n` → `60/n`, `0` → uncapped) and writes the new one, so an
existing config keeps the rate it had. And the settings page is **a slider whose
positions are a list**, which is the shape of the render-scale slider it sits
directly under.

That control has been three things. It was a combo of presets with a
`Custom...` entry that revealed a second slider, and it was the wrong shape for
the free number twice over — it made an arbitrary rate a *mode* to be selected
before it could be reached, and it put the ordinary case, a panel's own refresh
rate, behind an extra click for the sake of the rare one. So it became one
`SliderFloat` from 10 to 300 fps with the presets as **pins**: tick marks drawn
on the track, snapped to while a drag came within five pixels of one, the
tolerance in pixels rather than in fps and capped at half the gap to the next
pin, and the snap gated on the left button being held so that ctrl-click entry
and keyboard nav could still reach a rate by name.

**That was three mechanisms to make a continuous control behave like a discrete
one, and the discrete one was the answer.** Every integer from 10 to 300 was a
position the handle could land on, so the rates a player actually wants had to be
recovered by a magnet and then drawn back onto the track to say where they were
— and a value the player could reach but the magnet would pull off is a control
that argues with the hand. A frame rate is not a continuous quantity to a player
in the first place: it is the panel they own. So the slider is a `SliderInt`
over an index into the list — 20, 30, 40, 50, 60, 75, 90, 100, 120, 144, 165,
180, 240 — and it needs neither the magnet, nor the pixel tolerance, nor the
drawn ticks, all three of which are gone with the `Snap` and `DrawPins` bodies
that held them.

Two details make it read as a rate rather than as an index:

- **The format string is a literal**, `$"{Pins[_index]:0} fps"`, rather than a
  specifier. ImGui hands the format to `sprintf` with the slider's own value, so
  a format carrying no `%` prints the label and never the index — its own trick
  for a named-position slider.
- **Ctrl-click entry is off** (`ImGuiSliderFlags.NoInput`), because the box would
  open on the text `144 fps` and there is no specifier to read a number back out
  of it. A typed rate has nowhere to land on a slider whose positions are a list
  anyway; `KF2_FPS` is where a rate that is not on the list belongs.

`FramePacing` still takes an arbitrary double and that is still the point of the
patch, so a rate set outside the menu is **left running and said out loud**: the
slider re-reads `FramePacing` on every frame it is not being held, parks the
handle on the nearest position, and prints `Running at N fps, set outside this
menu.` under it whenever the two disagree by more than half a frame. Nothing is
written until the slider is moved. Pacing switched off entirely (`KF2_FPS=off`)
has no position on the scale either, so the handle parks at the world's tick
rate — 0 is not a rate any control here can express, and a handle at the far
left claiming 20 fps while the port draws unbounded would be a lie about what it
is doing.

### One switch for all of the smoothing

The smoothing arrived one patch at a time and the settings page grew the same
way: four checkboxes and two combos, one control per implementation part —
*Smooth the view between game ticks*, *Also smooth movement*, *Smooth other
things that move*, *Smooth model animation*, plus *Placement guard* under the
third and *Pose interpolation* under the fourth. Ten controls sat under
Video ▸ Enhancements and **four of them were this one idea spelled out in
parts**.

That is the implementation's shape, not the player's. Nobody wants the camera
carried between ticks and the creatures left stepping — the two smoothers were
made to agree about what time it is precisely so that they would not be used
apart (see "The camera is not the only thing that moves"), and a creature drawn
from a carried root and a stepping pose is the defect the per-table threshold
exists to avoid. The parts are separable because the *mechanism* needed them
separable while each was being built, which is a fact about the work rather than
a question to put to somebody playing.

So `patches/settings/FrameSmoothingPage.cs` is **one tick**: *Smooth motion
between game ticks*, which writes `FrameSmoothing`, `FrameSmoothing.Position`,
`ObjectSmoothing`, `AnimSmoothing` and `FluidSmoothing` together, and writes all
five of their `interface.ini` keys, so a restart restores what was clicked. The
parts stay reachable from the console — `KF2_SMOOTH`, `KF2_SMOOTH_POS`,
`KF2_SMOOTH_OBJECTS`, `KF2_SMOOTH_ANIM`, `KF2_SMOOTH_FLUID` — and that is now
the only way to set them apart.

**The tick sits under the frame rate rather than in Enhancements**, sharing the
`Frame pacing` heading with it. Everything else under Enhancements is a choice
about how faithful the picture is to the hardware; this one is a consequence of
the rate, and it is *inert* at or below the world's 20 Hz tick — at the shipped
60 fps the control a player meets is live. Its
explanation is the slider directly above it.

Three things about how it is wired:

* **The displayed state is `FrameSmoothing.Enabled`**, not an AND of the five.
  A config or an environment variable that turns one part on alone keeps doing
  that until the box is clicked, and clicking it harmonises them. The view is
  the master because it is the part that cannot sensibly be off while the rest
  are on: everything else is carried *against* the camera.
* **Each key is written from what the patch ended up with, not from what was
  asked for.** All four `SetEnabled` calls can refuse — `FrameSmoothing` and
  `ObjectSmoothing` need their hook pair (`_paired`), `AnimSmoothing` needs its
  clock sited (`_sited`), `FluidSmoothing` its DrawOTag pre (`_hooked`) — so
  writing the requested value would persist a claim that a part had attached
  when it had not, and it would come back next session still claiming it.
* **The two combos came out with the checkboxes they belonged to.** Both are
  comparison controls rather than preferences: their own write-ups say so, and
  say the losers go once the picture is judged. `KF2_SMOOTH_OBJECTS_GUARD` and
  `KF2_SMOOTH_ANIM=timeline|weight|time` still set them, which is where an A/B
  belongs.

**The position half is in the tick and carries a known inconsistency.** Two of
stage 13's own callees read the player position triple *after*
`FrameSmoothing.After` has put the raw value back — `func_80032400`, the
first-person arm, and `func_800331B4`, the world and object walks — so on a
non-tick frame those shear against the architecture by however far the carry
moved the eye, worst just before a tick lands. That is why it used to be its own
switch, off by default. It is inside the one tick now, and ships on with it;
dropping it back out is deleting the `SetPosition` pair in `Apply`.
Closing it properly means carrying those two readers too, inside the stage
`ObjectSmoothing` already brackets — `docs/TODO.md`.

**The merged tick is the shipped picture.** The parts had
each been judged separately and the position half had been judged *and rejected*
for the shear above; it ships inside the tick anyway.

## Frame pacing: the port is pinned to the fastest band

King's Field's game speed **is** its frame rate — everything advances a fixed
amount per loop iteration — and the loop always waits an integer number of
vblanks, so the achievable rates are quantised to 60/n. That quantisation is the
"banding" the game is known for: on NTSC hardware a frame costs 2, 3 or 4 vblanks
depending on scene load, so the game runs at 30, 20 or 15 fps and *plays* at
correspondingly different speeds. (PAL bands off 50 Hz instead — 25, 16.7, 12.5 —
which is where its ~17 fps ceiling comes from: more consistent, slower, and the
reason the PAL release feels different.)

Counting `VSync(0)` calls between consecutive `DrawOTag`s over a 30-minute
session, 49,570 rendered frames, is a direct measurement of which band each frame
landed in:

| vblanks charged | rate | frames | share |
|---|---|---|---|
| 1 | 60 fps | 6,960 | 14.0% |
| 2 | 30 fps | 42,460 | 85.7% |
| 3 | 20 fps | 30 | 0.1% |
| 4+ | ≤15 fps | ~120 | 0.2% |

**The port sits in the top band essentially always.** In an area it is 87% at
30 fps and 13% at 60 — the intro and title screens are ~99% at 60, because there
the loop only asks for one vblank. Nothing here is a throttle doing its job;
`DrawOTag` returns immediately on an HLE GPU and the MIPS is native code, so no
frame ever costs enough to fall into a slower band the way a real PlayStation
would under load.

**Read that last sentence again, because it is the whole trap.** The histogram
above is a measurement *of the port*, and the reason the 3-vblank band holds 0.1%
of its frames is precisely that the port cannot fall into it. It is not evidence
about the console; it is evidence that the port never does what the console did.

So the earlier framing of "twice as fast as the console" was too blunt. Precisely:
**in light scenes the port matches hardware's best case exactly, in heavy scenes
it is up to 2× faster because it never bands down, and for one frame in eight it
runs at 60 fps — twice the NTSC ceiling, which is faster than the game can go on
any console.**

### Walking between areas stuttered, and it was the smoothers re-priming

Reported from play as a hitch when *walking* between two areas and not when
teleporting. It is neither a stall nor a black screen, and the measurements that
say so are worth keeping because three plausible causes died on them: across
three crossings the picture held **165.0 fps drawn at 20.0 ticks/s**, with no
stalled frames, no frame-time gap (121 ms per 20 frames right through the load),
the display never masked off and at most a single black frame. The disc wait is
not even entered — no `KF2_LOADPACING_PROBE` line appears for a crossing — so
`LoadPacing` pacing it to 60 Hz, which is what a "hitch on a load" would be, is
not this.

What it is: **the smoothers stop carrying for two world ticks at every area
load**, so the picture steps twice at 20 Hz in the middle of a 165 fps stream
while the player is mid-stride. `patches/BlackProbe.cs` measured it per drawn
frame, as a run of frames the view was not interpolated into:

```
[black] fdat08 carry  -45 ms: . . . . . . . . . U U U U U U U U U U U
[black] fdat08 carry  +76 ms: U U U U C C C C C C C C C C C C C C C C
[black] fdat08: ... view unprimed on 15 frame(s), longest run 15 (~85 ms)
```

Both smoothers listened for `OverlayLoadedEvent` and cleared their pair —
`_primed = false` — because after a load the previous sample describes the old
area: the player has been teleported across the map, and a slot's previous tenant
is a different object. Carrying across that would sweep the camera through the
world and walk a creature in from a grave. The cost is that interpolation then
needs **two** fresh ticks, one for a sample and one for a pair, and those two
intervals render the raw tick value.

**Rebasing costs neither.** The load happens inside stage 2 or 3, both of which
are gated, so it always lands on a tick frame — and stage 8, where the camera pair
is rolled, runs later in that same frame, with the globals already holding what
the load left behind. So instead of clearing the pair, copy the fresh sample over
its own previous one: `prev = cur`. That interval then carries between two copies
of the post-load state, which is static and is exactly what an unprimed frame
would have drawn anyway, and the **next** tick already has a valid pair instead of
having to build one. `ObjectSmoothing.Sample` does the same per slot, and its
`Live` test is satisfied on the rebase tick rather than the one after it, so
nothing is swept in from a recycled slot's previous tenant either.

Measured on the same crossing: **15 frames / 85 ms → 6 frames / 33 ms**, and the
33 ms that remain are frames drawn *during* the blocking load, between the
overlay-loaded event and the next tick, where there is no fresh sample to rebase
onto and holding is the only correct answer. 144.0 fps drawn at 20.0 ticks/s,
`[present] wide 288, plain 0, vram fallback 0`, every hook attached, both before
and after.

**What this does not fix, and the reason it cannot be fixed by interpolating.**
The one interval that spans the teleport has no motion to interpolate: the
position either side of it is two different places in the world, and the only
thing that could fill it is an *extrapolation* from the player's own walk speed
(`0x80199558`) and heading. That is a guess about motion rather than a
measurement of it, and it is a feel judgement rather than a correctness one, so
it is not done here.

**The darkness that remains is the game's own fade, and the probe now says so in
its own column.** Play reported, after the rebase, "a moment of darkness or at
least not the correct frame" still at a crossing, and luminance alone cannot tell
a frame the game *painted* black from a frame that drew nothing. So
`patches/BlackProbe.cs` records two more things per drawn frame: the screen tint
the game asked for — `func_8003220C`'s request block at `0x80192D45`, `-` for
none — and a fingerprint of the sampled pixels, printed as `.` new pixels, `=` the
previous frame again, `2` the one before that (a flip to a buffer nothing finished
drawing) and `*` an older picture coming back. Measured over a crossing at 165 fps,
walking into it mid-stride:

```
[black] fdat05 frame   -186 ms: -. -. -. -. -. -. -. -. -. -. -. -. -. -. -. -. -. -. -. -.
[black] fdat05 frame     -3 ms: -. f. f* f= f= f= f= f= f= f= f= e= e= e= e= e= e= e= e= e=
[black] fdat05 frame    417 ms: b= b= b= b= a= a= a= a= a= a= a= a= a= 9= 9= 9= 9= 9= 9= 9=
[black] fdat05: 0 black frame(s) the game asked no tint for, longest run 0
```

The tint goes to `f` at the crossing and walks back down — `func_80037B5C(0x82,
0x1000, 0, -0x80)`, 32 steps of a quadratic tint held to the world tick, which is
**1650 ms** measured under `KF2_LOOPPACING=pace` and the same 1609 ms filled with
redraws by default. The disc read is under ~100 ms and is not what is on screen.
**Zero black frames in the window were untinted**, so nothing in it is an empty
frame; `[present]` stays `wide 3xx, plain 0, vram fallback 0` across every load,
the display is never masked off, and no short flash is reported. What the `=`
column shows is that the redraws between fade steps are byte-identical to the step
they follow, which is what a frozen world under a constant tint should produce.
That is the whole of the reachable evidence from counters: the crossing is dark
because the game fades, and the fade is 1.65 s because a modal loop runs at the
20 Hz reference. **Whether that reads as too long by eye is a play judgement and
is open.** It is also not the remaining report. Play still sees "a moment of
darkness or at least not the correct frame" at a walking crossing, and that
survives `KF2_LOOPPACING=0` and every smoother off, and it is **invisible at
`KF2_FPS=20`**. At 20 fps the port presents every 50 ms instead of every 6 ms, so
a one-or-two-frame artifact is simply never presented — treat "clean at 20" as
*the artifact is short* (~6–20 ms), not as already fixed.

The `frame` block cannot say a frame is *wrong*, only dark or repeated, so the
probe now writes the pictures. `KF2_BLACKPROBE=1` keeps the last 12 full display
rects in a ring and, on an `fdat` load (or a dark stretch long enough to mark
itself), the 250 ms after it as well, and writes them as PNGs under
`scratch/blackprobe/<overlay>-<timestamp>/` with an `index.txt` that carries the
same tint/fingerprint/carry columns as the console dump. `KF2_BLACKPROBE_OUT=dir`
overrides the folder, `=0` skips the pictures. The read is the same
`ReadVram` path, of the display rect after the target has been written back, and
it skips 0039's restore copies — a diagnostic that sampled every frame *with* the
default snapshot would evict the copy a menu is restored from, and would itself
be a candidate for the wrong frame.

**Captured on a walking crossing.** `fdat05 → fdat02` at ~144 fps, mid-stride
(`warp` is `func_80024154` and is not this path):

```
[black] flash: 1 frame(s), ~7 ms, buffer y0, other buffer 47, 1 present(s) during, back to 47
[black] fdat02: 1 black frame(s) the game asked no tint for, longest run 1
```

The PNG is `scratch/blackprobe/fdat02-20260920-151646/13_+013ms_000_-..png`:
**HP/MP and the compass on a black world**, 13 ms after the overlay load,
display `y=0`, no tint, fingerprint `.` (new pixels, not a stale flip). The
frame before (`12_+006ms`, `y=240`) and the frame after (`14_+020ms`, `y=240`)
are the previous room, other-buffer luminance 47. `[present]` stayed
`vram fallback 0`. The three-row luminance of 0 is the world rows at 60/120/180;
the HUD sits above them, which is why a counter called it black and the picture
is not an empty framebuffer.

The other direction of the same walk (`fdat02 → fdat05`) did **not** flash:
frame 13 there is a real corridor at lum 56. So it is not every crossing, and it
is not the 1.65 s fade (this path never asked for a tint). Stage 13 submits sky,
HUD, then tiles, then models; this OT had the HUD and not the world.
`PutDrawEnv` has `isbg=0`, so a missing tile walk would have left the previous
picture under the HUD — something filled the back buffer first, then the HUD
drew, then this was presented. Invisible at `KF2_FPS=20` because that DrawOTag
is one 7 ms present.

### What the crossing frame actually is

Everything below is measured this session, at `KF2_FPS=144`, walking area 1 into
area 0. **Two fixes were shipped at this defect before any instrument could see
it, and both were aimed at the wrong layer**; the point of this section is that
the negative results are the useful part.

**The repro everything was tested against does not reproduce it.** A `goto` onto
the boundary object drops the player on the trigger from a standstill. Measured
over that crossing: 375 frames across a ±2.6 s window, all at 144 fps with no
gap, **0** frames drew no map, **0** drew a half-built one, no frame drawn from a
stale camera, and no black frame at all. Every hypothesis in this document's
history was tested against that, which is why each of them looked plausible and
none of them held.

**Walking in reproduces it, deterministically, on every crossing.**
`scripts/`-driven: stand short of the boundary, solve for the yaw that faces it
(hold `Up` briefly from a known yaw, read the displacement, rotate), then hold
`Up` through it. Every walked crossing so far:

```
[black] flash: 1 frame(s), ~7 ms, buffer y240, other buffer 46, 1 present(s) during, back to 46
[black] fdat02: 1 black frame(s) the game asked no tint for, longest run 1
```

One fully black frame, ~7 ms, no tint requested, **new** pixels rather than a
repeat, with the picture at luminance 46 either side of it.

**The frame that goes black is a frame that was fully built.** `KF2_BLACKPROBE=1`
now carries two more columns per present — whether the renderer ran for it, and
the cells the tile walk submitted — so the bad frame is read off one table
instead of correlated between two probes. Its row, column 15:

```
[black] fdat02    -84 ms: |  46 |  47 | ... |  46 |   0 |  46 |  46 |
[black] fdat02 frame  ... : -. -. ... -. -. -.
[black] fdat02 built  ... : R199 R199 ... R175 R175 R175
[black] fdat02 carry  ... : C C ... C U U
```

luminance **0**, tint **`-`** (the game asked for none), drew **`.`** (new
pixels), built **`R175`** (the renderer ran and the tile walk submitted 175
cells), carry **`U`**. A complete ordering table went in and a black picture came
out. That rules out the entire "the frame lost its world" family, which is what
both shipped fixes attacked.

**Four things it is not**, each with the counter that says so:

| ruled out | how |
|---|---|
| the ordering table lost its map | `KF2_CROSSPROBE=1`: 0 frames drew no map, 0 drew a half-built one, over 573 presents |
| the area module swapped mid-table | real — 8 of 8 crossings — but holding the CD pump across the renderer moved it to **0 of 8 and the black frame did not change** |
| a flip to a buffer nothing drew | a since-removed `KF2_FLIPPROBE` kept a bit per display buffer, cleared by `PutDrawEnv`'s `isbg` and set by the `DrawOTag` that follows: **no event anywhere near the crossing**, and 0 of 573 presents were ones the renderer did not build |
| the port's rendering stack | with `KF2_POLYASM`, `KF2_TILEWALK`, `KF2_MODELWALK`, `KF2_ZBUFFER`, `KF2_AO`, `KF2_PERPIXEL`, `KF2_EVENFOG`, `KF2_SUBPIXEL` and `KF2_PERSPECTIVE` all `0`, the flash is still there |

**Three traps in the instruments, all of which produced a confident wrong answer
before being caught.**

- **Luminance cannot see it.** `BlackProbe` asks whether a frame is *dark*, and
  the door opens into an unlit room. Both arms of an A/B reported the same seven
  or eight one-frame `[black] flash` lines at luminance 3 on both buffers — that
  is the room, not the defect, and it hid the real single black frame for two
  whole investigations.
- **A cell count needs a per-area baseline.** Comparing each frame against the
  *window's* peak called **285 of 371** frames half-built, when every one of them
  was right: area 1 steadily draws ~200 cells and area 0 draws 71 at one door and
  ~180 at another. The peak is taken per area byte now.
- **Sampling in the renderer's post cannot see a present it did not build**, and
  `DrawOTag` fires more than once a frame — 143 of 214 presents in one window were
  not renderer output. A probe that wants to describe *what was shown* posts on
  `DrawOTag` and records whether the renderer ran, which is what the `built`
  column is.

**It is `FrameSmoothing`, and specifically the position carry.** Bisected by
switch, two walked crossings each:

| | flashes |
|---|---|
| default | **1 per crossing** |
| every rendering enhancement off | 1 per crossing |
| `KF2_TINTHOLD=0` | 1 per crossing |
| `KF2_SMOOTH=0 KF2_SMOOTH_OBJECTS=0 KF2_SMOOTH_ANIM=0` | **0** |
| `KF2_SMOOTH=0` alone | **0** |
| `KF2_SMOOTH_POS=0` alone | **0** |

So the camera smoother's *position* interpolation is necessary for the artifact,
and the angles are not. Note that `KF2_SMOOTH_POS` is documented as off by
default and is **on** in a saved config, so the shipped behaviour is the one that
flashes.

**Two structural fixes were proposed against that and both are falsified**, which
is why the next one should be measured before it is believed:

- **Guarding the angles across a teleport.** The position carry has had a
  `TeleportUnits` guard from the start and the angles never did, so the crossing
  frame is drawn from the new area's position with a yaw part-way between two
  rooms' facings. That is a real asymmetry and the guard fires four times a
  crossing — **and the black frame is unchanged**. Reverted.
- **The rebase claiming a pair it does not have.** `_rebase` sets `prev = cur`
  and then `_carriable = true`, which is one sample wearing two hats, so carrying
  resumes immediately after a load. Setting `_carriable = false` there so the next
  tick builds a real pair — **black frame unchanged**. Reverted.

**What the trace says, and it is the thing to start from.** `KF2_SMOOTH_PROBE=2`
prints this class's decision and its writes for 400 ms after an overlay load. At
a crossing it reads:

```
verdict 3 tick 0 frac 0.21 applied 1 ... pos (161001,94856) prev (161001,94886) cur (161001,94743)
verdict 1 tick 0 frac 0.48 applied 0 ... pos (153587,91371) prev (153587,91371) cur (153587,91371)
```

Two things in there are worth more than any theory so far. **A crossing makes two
placements, not one** — the module loads and the player lands at ~(161001,94900),
and only then is moved to (153587,91371) — and **the frame that goes black is the
second one, on which this class carried nothing at all** (`applied 0`). So the
damage is not done by the write on that frame; it is done by something a carried
frame leaves behind, or by the second placement landing on a frame the carry has
already influenced. That is where the next attempt should look, and it should
look at the trace before proposing a mechanism.

**A third hypothesis is falsified: the walks and the camera disagreeing.** Stage
8 reads the carried position, but `func_800331B4` (the world and object walks) and
`func_80032400` (the arm) read the triple after `After` has put the true one back
("Stage 8 is the render camera" in [GAME_INTERNALS.md](GAME_INTERNALS.md)); at a
crossing the two could sit in different areas. Tested by moving the restore from
stage 8's post to a post on the renderer `func_800342D8`, so everything in stage
13 saw the carried position: **1 black frame on each of 2 walked crossings**,
~6-7 ms, luminance 46 either side, the same as without it. The player still
landed at (153587, 91371) both times, which a restore that never ran would not
have allowed. Reverted. That fits the trace above: the black frame is one on
which nothing was carried, so there was no disagreement on it to remove.

**Found: the smoother lerped the old room over the new position.** The instrument
that named it is a `camgap` row in `KF2_BLACKPROBE=1`: for every present, the
camera the renderer actually used (`func_8002E22C`'s copy at `0x80192E78`/`E80`,
read as the renderer returns) against the true position, and whether stage 8 ran.
Every frame of a walk reads a gap of 0-143 units, the carry's own sawtooth; the
black frame reads **133,008**, camera (28557, 69513) in the old area and the
player at (161560, 96020) in the new one, with stage 8 having run. With
`KF2_SMOOTH_POS=0` the gap is 0 on every frame. An ordered log of stage 8's
pre and post, the renderer's entry and exit, and the load then gave the sequence:

1. The game writes the new position **before** `OverlayLoadedEvent` fires; the
   event lands a frame later, mid-renderer, which is the mid-table swap already
   recorded above.
2. So on that frame `_rebase` is not set yet, and the pair is two old-area samples,
   (28582, 69539) to (28440, 69396), 142 units apart, which the teleport guard
   passes. The guard compares the pair with itself, never with what is in memory.
3. `Before` writes the lerp over the new position, stage 8 copies it into the
   camera, `After` puts the new position back, and the walks draw the new area
   around the player while the camera looks from 133,000 units away. Black.

The earlier conclusion that the black frame was one "on which this class carried
nothing" was the trace line one frame late: the `verdict 1 ... applied 0` line is
the frame *after*, once the event has set `_rebase`.

**The fix is the check the guard was missing.** Off a tick the live view must
still be `cur`, since the game only moves the player on a tick; if the position
or an angle differs, the game placed the player between ticks, and the pair is
rebased onto the live values instead of carried (`FrameSmoothing.Moved`,
since shifted rather than rebased — below —
`OffTickMoves`). Measured over two walked crossings: **0 black frames** against 1
per crossing, no `camgap` over 1,024 anywhere, and the guard fired 5 times in the
session, each at a `goto`, a `warp` or a crossing. Ordinary walking still
carries the position (71.8 u a carried frame). Confirmed from play: the flash is
gone.

**Rebasing onto the placement lurched the view forward, twice.** The smoother
draws up to a tick behind the game, and `prev = cur = live` drops that lag in one
frame and then holds still until the next tick. The late `_rebase` then did it
again at that next tick. Reported from play as "jumping forward on the
transition"; a `camstep` row in `KF2_BLACKPROBE=1` (the camera's move per present,
with a placement's offset taken out) read, across the crossing:

```
20 20 20 … 20  136  0 0 0 0 0  143  0 0 0 0 0 0 0  21 19 20 …
```

A crossing's placement is a pure offset — (133120, 26624), whole 2048-unit tiles
— so the pair is now **shifted** by it rather than reset: `prev` and `cur` both
move by `live - cur`, and the view carries straight through. A placement that
lands on a tick instead puts `prev` one walk step (the last tick's) behind `cur`.
`_rebase` is gone; the event only opens the trace window. Measured, two walked
crossings: `20 20 22 21 18 24 21 17 | 20 21 19 20` through the crossing, no black
frame. Confirmed from play: the crossing is smooth.

What is left is the game's. About 200 ms after the crossing the camera moves at
35-40 a present for one tick: the tick's step is (127, 270) against (142, 143)
either side, the doorway's collision sliding the player along the wall. With the
smoothing off it is the same distance in one 20 Hz step. **The "second
placement" above is not part of the crossing**: (153587, 91371) is where the
harness's `warp` back found the player, and the trace window it opened was read
as the crossing's.

### A dropped DrawOTag presents the background clear

The first attempt at the crossing frame held the *symptom*: a pre on `DrawOTag`
returned false when the ordering table had been built across an `fdat` swap, on
the premise that `PutDrawEnv` has `isbg=0`, so the draw buffer still held the
previous room and skipping the submit would present that room again.

**It made every crossing worse, and the premise is the reason.** Measured, 16
crossings each way at 144 fps: with the drop on, 16 of 16 crossings dropped a
`DrawOTag` and 16 of 16 presented a **fully black** frame — HUD and all, which is
worse than the artifact it was hiding; with it off, 0 and 0. The port does not
leave that buffer alone: `PutDrawEnv` is logged issuing both a 320-wide `isbg=0`
env and a 640-wide `isbg=1` one, and the widened `isbg` rectangle is the *only*
thing that paints the widescreen margin every frame (`0022`–`0024`, "The margin's
only clear is the game's own" in [WIDESCREEN.md](WIDESCREEN.md)). Skipping the
submit therefore presents a freshly cleared buffer, not a kept picture.

The general shape is worth keeping: **a hold that keeps a frame has to know what
put the pixels there**, and in this port that is not always the game.

`FrameSmoothing.Frames`, `Carries` and `LastVerdict` are public so a probe can ask
per frame whether the view was carried and why not; `KF2_SMOOTH_PROBE=1` still
reports the same thing per second. `AnimSmoothing`'s listener is deliberately
untouched — it clears cached clip lengths because the banks are re-linked by a
load, so a cached length is a read of someone else's memory, which is a
correctness reset and not a priming one.

### The reference band is 3 vblanks, not 2

**This overturns what the rest of this section originally concluded**, which was
that the reference speed is the top band, 30 fps, "which is both the design
ceiling and where the port already spends 87% of its frames". The second half of
that was circular, per the paragraph above. The first half confuses two things:

* **What the code permits.** Two vblanks, the literal `2` at `0x800178A4`. That
  is a reading, it is confirmed, and it has not changed — see "The loop's own rate
  gate" in [GAME_INTERNALS.md](GAME_INTERNALS.md). **Confirmed** from the other end
  too: `func_80018690` opens the vblank event
  (`OpenEvent(0xF2000003, 2, EvMdINTR, 0x80017850)`) and `func_80017850` increments
  the credit at `0x801B6CA8` once per vblank, so the gate's `< 2` spin is two
  vblanks of *wall clock*.
* **That literal is a ceiling, not a target, and the difference matters.** The
  gate spins *while* the credit is below 2: it forbids a frame faster than 30 and
  asks nothing whatever of a slower one. So it is a speed limit, and a limit that
  was never the binding constraint is not evidence of an intended speed. Calling 30
  "what the code asks for" — which this document and the source comments did — reads
  as design intent and claims more than the instruction supports.
* **What the console delivered.** King's Field is heavy enough that the loop
  misses that deadline under load and the frame costs three vblanks. Since the
  game's speed *is* its frame rate, the band it actually lands in is the speed the
  game was played at — 20, and by the paragraph above that is the only one of the
  two numbers with a claim to being the speed it was *built* at.
* **Open, and reported rather than measured here:** that the original Japanese
  release, *King's Field II* `SLPS-00069`, is itself capped at 20. If that is a
  three-vblank gate in the JP executable it would settle the question outright —
  the same game shipped with 20 as its stated ceiling, and the US `2` a regional
  change rather than the design. **Not checked**: this project has only
  `SLUS-00158`, whose gate reads `2`, and nothing here can read a disc it does not
  have. Worth an hour with the JP image if one is ever to hand — the address to
  look at is that literal.

A port that makes the 2-vblank deadline on every frame therefore plays the whole
game **half again as fast** as the console did, not "the same as hardware's best
case". The reference is **20**, and it is now the default: `FramePacing.LogicHz`.

**No counter here can settle that**, and it should not be presented as though one
did. The port cannot observe hardware, and the histogram above is the shape of
evidence that looks like it can and does not. It is a judgement about the console
— **and a judgement the port has to make, not hand over**. It was a combo under
Video offering 20 and 30, and that combo is gone: 20 is the rate, every measurement
below was taken at both, and 30 is now reachable only as `KF2_TICKRATE=30`, which is
where a comparison belongs. `kf2.framepacing.logichz` is no longer read either, so a
config left saying 30 does not quietly run a session half again too fast with
nothing in the window to explain it.

**Step one is a floor, not a scale factor.** Enforce a minimum of two vblanks
(33.3 ms) per rendered frame and the port is a constant 30 fps: exactly NTSC's
fastest band, never above it, and without the banding that made the original's
speed wander. No game knowledge, no constants touched — and it is strictly more
consistent than hardware ever was. **Done**; see below.

### The floor, as built

`patches/FramePacing.cs`, attached as a post hook on `DrawOTag` in all three
overlays through `HookManager` at run time. Three things about the shape of it
are worth keeping:

**It is not a change to `FrameClock`.** The runtime's throttle paces per *`VSync`
call*, and from inside it a one-vblank frame is indistinguishable from the first
half of a two-vblank one — the information it needs (where the frame ends)
arrives at `DrawOTag`, which `FrameClock` never sees. The floor therefore keeps
its own deadline and lets the two clocks coexist: for a frame the floor extends,
`FrameClock`'s deadline simply falls behind real time and its wait collapses to
nothing (it resyncs on the `wait < -100` path every few frames), so the floor
ends up the sole pacer for exactly the frames that need one, and a no-op for the
frames that were already two vblanks long.

**It needed no RecompOne patch and no config entry.** `SymbolRegistry.Resolve`
turns `("game", 0x80060818)` into the emitted method and `HookManager.AddPost`
detours it, so the hook is installed at run time. It composes with the `replace`
that binds `LibGpu.DrawOTag`: `HookManager.Invoke` runs pres, then the
replacement, then posts.

The attach is deferred to the first `OverlayLoadedEvent`, because the dispatcher
tables `SymbolRegistry` reads are registered inside `Entry.Run` — after
`Program.cs` has run, but before anything is loaded, so the first load event is
the earliest moment every overlay resolves.

Note the namespace trap: generated code is `Recompiled.KingsField2`, a *class*
named after the project, which shadows any namespace called `KingsField2` — hence
`Kf2`.

**The vblank defines the frame boundary, not the `DrawOTag` call.** A second
ordering table with no `VSync` between it and the first belongs to the frame
already in flight; charging the floor per call would halve the rate of any screen
that draws more than one OT. King's Field draws exactly one (no zero-vblank gap
appears in any measurement), but the guard is free — the hook counts `VSync` calls
off a pre-hook on the libetc thunk and returns early when the count is zero. (It
counted *vblanks* until the fix described under "Any frame rate"; that is what made
every rate above 30 run the game fast.)

The deadline is absolute rather than `now + 33.3`, so a frame that overruns is
paid for out of the next one instead of the rate drifting down by the accumulated
jitter, with one frame of debt as the limit: past that the game has *stopped*
drawing (a disc read, a module swap) rather than run late, and the cadence
restarts instead of running flat out to catch up.

Three env vars, all read in `Program.cs`:

```bash
KF2_FPS=20          # 60 fps is the default; 20 is the world's tick and a position on the slider
KF2_TICKRATE=20     # ticks a second the world runs at; 20 is it, and 30 is the comparison
KF2_FPS_GATE=80037C0C+8002A550+80040348+80046A60+8004910C+80033FBC+8002DC78   # what is ticked
```

(That gate list *replaces* the default set rather than adding to it, and the rate
is an arbitrary double rather than a vblank divisor — see "Any frame rate" below,
which supersedes the two-stage, every-other-frame scheme this paragraph used to
describe.)

The band table above was measured by a `framestats` mod (since removed) — the same
count of `VSync(0)`s between `DrawOTag`s that fed it, without the gigabytes of
`KF2_LOG=sdk`. Bands were reported per window, so consecutive lines separated the
title screen from an area; restoring the mod is the way to check a pacing change.

### What the floor actually changed — and a correction

Measuring with `KF2_FPS=off` makes the deviation look different
from the aggregate above, and more specific. In an area the port is **already at
exactly 30 fps, asking for two vblanks on every single frame**, for minutes at a
time — then it flips into a burst where it asks for one:

```
floor off                                        floor on
30.0 fps  450 frames  2:100.0%                   30.0 fps  300 frames  2:100.0%
28.3 fps  424 frames  1:8.5%  2:91.5%            30.0 fps  300 frames  2:100.0%
30.0 fps  450 frames  2:100.0%                   30.0 fps  301 frames  2:100.0%
39.1 fps  587 frames  1:83.8% 2:14.0%  ...       30.0 fps  301 frames  2:100.0%
51.6 fps  775 frames  1:87.7% 2:11.0%  ...       30.0 fps  300 frames  2:100.0%
30.0 fps  451 frames  2:100.0%                   30.0 fps  300 frames  2:100.0%
```

So the one-vblank frames are **not spread evenly through play** the way "14% of
frames" implies — they cluster, and while a cluster lasts the whole game runs at
about 1.7× speed for half a minute at a stretch. That is a worse bug than a
uniform overspeed and an easier one to feel: the world lurches, then settles.

The floor removes them. Over 3,200 frames of the floor-on run **no report window
came out above 30.0 fps**, and eight consecutive in-area windows were 30.0 fps at
`2:100.0%` exactly. Windows *below* 30 stay below it — the title screen and the
intro are CD-bound at 7–15 fps with the floor on or off, which is right: a floor
is a ceiling on speed, not a promise of one; the music that ran half speed there
was the vblank domain ticking at picture rate — see "The vblank fired when the
game asked" in `docs/RUNTIME.md`.

Two things the measurement says that are worth keeping in mind for step two:

- **The game asks for two vblanks on its own almost all the time.** Whatever
  decides that count is not measuring host time; it is the game's own pacing
  logic, and the port's job is only to stop it going faster than the top band.
- **The band histogram is unchanged by the floor** in steady state (`2:100%` in
  both columns), which is the evidence that the floor is not fighting the game's
  loop or the runtime's `FrameClock` — it just absorbs the slack.

**Step two was 60 fps, and it turned out to be a different shape than this.**
What follows replaces the plan sketched here: the thing to remove is the game's
own frame gate rather than a vblank floor, and the world is held to a rate by a
clock rather than by halving deltas. **And the reference rate is not 30** — see
"The reference band is 3 vblanks, not 2" above. See below.

## Any frame rate: three gates, one logic clock, and a smoothed view

**Confirmed mechanism throughout; the picture has not been checked by eye.**

### The frame boundary was the vblank, and that made every rate above 30 run fast

Recorded first because it is the mistake the rest of this section was written
around. `FramePacing.AfterDrawOTag` opened with `if (_vblanks == 0) return;`,
counting `VSyncEvent` — which since `0021` is **the emulated vblank on a fixed
wall-clock 60 Hz grid**, not the game asking to present. That is once a frame only
while a frame lasts at least 16.7 ms. Above 60 rendered fps most frames reached
`DrawOTag` with no vblank elapsed and were thrown away: no `Floor()`, no
`AdvanceLogicClock()`, not counted in `Measured`. Two things followed.

* `_tickThisFrame` kept the **previous** frame's value, so the frame after a
  ticking one ran the gated stages again. With N frames per vblank the world ticked
  at `30 × N`.
* `Floor()` ran at most 60 times a second while advancing its deadline by
  `1000/target` each time, so above 60 it fell a frame behind on every call, reset,
  and stopped throttling. `FrameClock`'s permissive `2 × target` ceiling — a
  *ceiling*, never meant to pace anything — became the real limiter, which is why
  the rendered rate came out at exactly twice what was asked for.

Measured in an area, slot 2, before the fix:

| `KF2_FPS` | rendered | world ticks/s | death clock (65 ticks) |
|---|---|---|---|
| 30 | 30 | 30.0 | 2.13 s ✓ |
| 60 | **120** | **~59** | **1.10 s** |
| 120 | **240** | far above 60 | — |

The rate-independent form of the same "don't charge a second ordering table
twice" rule is **the ordering table drawn after the game asked to present**, so
the boundary is now the `VSync` *call*: a pre-hook on the libetc thunk
(`open 0x8001EB88`, `game 0x8005FCC8`, `end 0x8001B154`) counts calls and
`AfterDrawOTag` returns early only when the count is zero. Above 30 the game's own
frame gate is skipped, so a frame carries exactly one call — the presenter
`func_8002E0FC`, which does `VSync(0)` immediately before `DrawOTag`. At 30
nothing changes: the presenter's call still lands before the OT and the frame
gate's spin calls land after it. After the fix, every rate from 30 to 144 measures
**30.0-30.3 world ticks a second and a rendered rate equal to the number asked
for**.

`mods/framestats` carried the same guard and so reported the vblank-bearing subset
as its frame rate; it now uses the call boundary too, and a `0:` band in its
vblanks-per-frame histogram is the correct answer for a frame shorter than a
vblank.

### Everything hung off one hook, and losing it read as "the whole game runs fast"

The boundary above is a **single point of failure**, and until this was written it
was an unannounced one. `Floor()` has exactly one call site — the boundary — and
`_tickThisFrame` is written in exactly one place — the boundary. So a boundary that
stops arriving takes both halves at once: the picture goes uncapped *and*
`_tickThisFrame` freezes at whatever it last held. Its initial value is `true`, so
the freeze runs every gated stage on every frame and the world advances at the
render rate. At the rate this port is actually played at (165, against a 20 Hz
world) that is **8× speed**, from the title screen onward, with nothing said on the
console. That is the shape of the intermittent "the whole game runs fast" report,
and no other single failure in `patches/` produces both halves together.

Three things now stand between that and the player. None of them changes anything
when the boundary is healthy — measured, `walk` reports **2844 units per 2 s at 165
fps** with the boundary intact and **2844 with all three `DrawOTag` hooks
deliberately removed**.

* **The stage gate refuses to believe a stale boundary.** A gated stage is only
  ever reached from the main loop, and the main loop draws, so a stage running
  while no boundary has arrived for `BoundaryDeadMs` (500 ms — past the 250 ms the
  logic clock already treats as a stall, and past any disc read, since a blocked
  game is not calling stages either) means the boundary is not being reached at
  all. `FramePacing.FallbackTick` then runs the same wall-clock grid from inside
  `BeforeStage`, paces the loop there, and says so once. One decision is held for a
  quarter of a tick because the seven gated stages of one iteration have to agree
  with each other — half an iteration ticked is a state machine stepped against an
  entity table that was not.

  **That hold doubled the frame rate, and it was the watchdog's one active defect.**
  The hold was the whole test, so once an iteration took less than 3 ms — every one
  above ~330 fps — the next iteration reused the previous decision: no `Floor`, two
  frames to one wait, the picture at the host ceiling (measured 483 presents a second
  at 240, 235 fresh decisions against 3144 held), and a tick decision run twice. A
  decision is now held only until the next present, read off `LibEtc.VSyncCalls`
  (`patches/recompone/0042`) so it does not depend on the hook whose loss put pacing
  here. Measured with GAME.EXE's `VSync` hook removed: 240.0 presents and 19.7-20.7
  ticks a second, 240 fresh against 1440 held. `KF2_FPS_PROBE=1` now prints a
  `no boundary` line from the vblank while this runs, since its usual line comes
  from the boundary that is missing. How the boundary was actually being lost —
  the tiered JIT recompiling a hooked thunk under MonoMod's detour, fixed by
  `TieredCompilationQuickJit=false` — is in docs/TODO.md; `FramePacing`'s sentinel
  prints `[KF2] pacing sentinel:` with a `VSync` stack if a pacing hook is lost again.
* **No `VSync` thunk is a coarser boundary, not the absence of one.**
  `_boundaryNeedsVSync` clears when none could be hooked, so every `DrawOTag` is
  charged as a frame. This game draws one ordering table per frame, so that is
  simply right here; a frame carrying two would be paced twice, which is the
  failure worth having. It is **recomputed at the end of every pass** rather than
  only ever cleared: `Attach` is built to be retried, so a first pass that got
  `DrawOTag` and no thunk and a second that got all three used to stay permanently
  in the degraded mode while the summary line reported `3/3 VSync`, with nothing
  pointing at it.
* **The attach says what landed, and is retried.** `Event.Dispatch` wraps every
  listener in a `try`/`catch` that writes one line to stderr, and
  `HookManager.Commit`'s `foreach` used to abort on the first detour that would not
  install and take every function after it — so a partial attach was silent *and*
  permanent, the latch already set. `Attach()` now claims only what it **installed**
  (see "A registration is not a hook" below), returns whether it is complete, and is
  retried on the next overlay load up to `HookAttach.MaxTries`;
  `patches/recompone/0027` commits each function on its own so one failure cannot
  take the rest. The pacing line spells the boundary out as a pair rather than
  folding it into a total, because reading `15 hook(s)` and counting on your
  fingers is how this went unnoticed:

  ```
  [KF2] pacing: 165 fps, 15 hook(s), boundary 3/3 DrawOTag + 3/3 VSync,
        7/7 stage(s), frame gate skipped, world at 20 Hz gating 80037C0C …
  ```

A related and separate defect fixed at the same time: `AdvanceLogicClock` treated
`dt <= 0` as "restart the clock", which **granted a free tick** to any two
boundaries landing on the same reading of the `Stopwatch`. `LoopPacing`'s fill
issues boundaries back to back with almost no work between them, and its own exit
condition is `TickedThisFrame`, so a spurious tick both advanced the world and
ended the fill early. Only the genuine first sample restarts now; no elapsed time
means no credit and no tick.

### The rate a session is actually running at (`KF2_FPS_PROBE=1`)

Every other probe in the port hangs off a *game* function, so none of them says
anything before an area is loaded — and a session that is going to have no
smoothing in it announces itself long before that, at the title, by running at the
wrong speed. `KF2_FPS_PROBE=1` is driven by the frame boundary instead, which is
reached in all three executables, so it reports from the first second of the boot:

```
[KF2] pacing: 165.0 fps drawn of 165 fps, 20.0 tick(s)/s of 20 Hz, smoothing can
      carry; view carrying, objects carrying, anim idle
```

Three numbers, a verdict, and three health words. The numbers are the rate
achieved against the rate asked for and the ticks actually taken against
`LogicHz`. The verdict is `FramePacing.Extrapolating`, which is the single test
both halves of `FrameSmoothing` and all of `ObjectSmoothing` and `AnimSmoothing`
sit behind.

**The verdict alone was not enough, and finding that out is what added the three
words.** `Extrapolating` is `Gating && (!Enabled || TargetFps > LogicHz)` — a pure
function of `LogicMode`, `Enabled`, `TargetFps` and `LogicHz`. It is
*configuration*, not a reading: it says the smoothing **may** carry and can never
say whether it **did**. Neither of the other two fields reads `LogicPhase` or
mentions a smoother at all. So a session with a dead smoother printed a line
byte-identical to a healthy one, which is exactly what play reported when this
probe was pointed at the open question below — the probe was aimed one layer too
high, at `FramePacing` rather than at the patches whose picture was missing.

Each patch now answers for itself, through `TakeHealth()`, which consumes the
window it reports on. The counter behind it is incremented whether or not that
patch's own probe is switched on, because the session that needs this reading is
precisely the one nobody thought to enable three extra probes for. The words are
ordered by how far the patch got, so the first one that is not `carrying` names
the layer:

| word | meaning |
|---|---|
| `unpaired` / `unsited` | the detour never installed. `SetEnabled` is `on && _paired`, so the **Video checkbox can never turn it on** for the rest of the session; `Attach` said so once, on stderr, and nothing mentions it again |
| `off` | the saved setting or the env var reads off |
| `at the tick rate` / `not gating` | no frame lands between two ticks |
| `unprimed` | attached and on, but two ticks have not been sampled since the last `OverlayLoadedEvent` — which fires for the `fdat` area modules as well as the executables |
| `idle` | running, with nothing moving to carry |
| `carrying` | the healthy reading |

`anim` has no `unprimed`, and **that absence is the finding under it**:
`AnimSmoothing` primes **per slot** (`Slot.HasPrev`, re-seeded from the clip
record the next time the slot is seen), where both siblings gate every frame on a
*single global flag* cleared by `OverlayLoadedEvent`. That is the one structural
asymmetry between the three patches — their `Install`, `Attach`, settings read and
`_paired` guard are otherwise the same shape — so it is the first thing to suspect
whenever `anim carrying` is printed beside two that are not.

Measured on this machine at `KF2_FPS=165`: `view unprimed, objects unprimed, anim
idle` through OPEN.EXE and the intro movie (stage 8 and stage 13 are GAME.EXE's,
so they are never reached there and the word is honest rather than a fault);
`view idle, objects carrying, anim idle` standing still in an area with nobody at
the keyboard; and `view carrying` from the first window in which `KF2_SHELL`'s
`press Left` turned the camera.

**The baseline, measured over a 200 s boot on this machine, is:**

| where | fps drawn | ticks/s | why |
|---|---|---|---|
| logos (OPEN.EXE) | **15.0** | 15.0 | disc-paced STR stream, so not moved by `KF2_FPS` |
| title menu (OPEN.EXE) | **60.0**, or the asked rate below it | same | no wait of its own; `LoopPacing` holds it to `InterfaceHz` — see "The title menu is an interface frame" |
| the intro movie | **10.0** | 10.0 | disc-paced, `patches/recompone/0026` |
| an area, attract demo or play | **165.0** | 20.0 | the render rate asked for, against `LogicHz` |

**The title row was wrong for a long time, and it is the one worth knowing.** It used to read 15.0, "OPEN.EXE's own four-vblank wait" — but that is the logos' stream. The title menu's loop (`func_80011AE0`) has no wait of its own, one `VSync(0)` and one `DrawOTag` a picture, so it read whatever `KF2_FPS` asked until it was capped at 60 like the menus. A title menu at **twice** the asked rate is therefore not the game: it is `FramePacing.ApplyHostCeiling`'s `2×` ceiling with nothing of the port holding the picture, which is the boot the sentinel is for (see "The smoothing is sometimes dead for a whole session" in [TODO.md](TODO.md)).

`present(s)/s` on the line is counted inside `LibEtc.VSync` (`patches/recompone/0042`), not by a hook, so it can disagree with `fps drawn`: legitimately in a fade, which `VSync`s without drawing (62-131 a second measured), and illegitimately when a pacing hook has stopped running. **The sentinel** reads the same counter from `VSyncEvent`, which fires whether or not a detour does; two seconds above `max(1.5×T, 90)` print one `[KF2] pacing sentinel:` block — hook fires, `Floor` calls and waits, `FallbackTick` fresh and held decisions, what is committed, a one-line reading, and the stack of one `VSync` call — once a session.

Otherwise the probe names the cause: a low `fps drawn of` says the saved rate did not load, a `nothing to carry` says the rate itself leaves nothing to interpolate, a `tick(s)/s` far above `LogicHz` in an area is the lost-boundary failure the section above describes, and any health word other than `carrying` or `idle` in an area says which patch is inert whatever its checkbox reads.

### A registration is not a hook, and seven patches latched before doing the work

The bullet above was written for `FramePacing` and was **not true even there**, for
a reason that then turned out to apply to every patch in `patches/`. Two separate
mistakes, both of which report success:

**The latch was set before the work.** Every patch here attaches from an
`OverlayLoadedEvent` listener, because `SymbolRegistry` is only readable once the
dispatcher's overlay tables are registered. Seven of them — `FrameSmoothing`,
`ObjectSmoothing`, `AnimSmoothing`, `MenuPacing`, `LoadPacing`, `LoopPacing`,
`SpriteAnim` — wrote `attached = true` and *then* called `Attach()`. Anything
thrown inside it — a renamed method behind a null-forgiving `GetMethod(...)!`, a
`SymbolRegistry` failure, a detour MonoMod will not install — is swallowed by
`Event.Dispatch` into one stderr line, so the patch was left permanently
half-installed, never retried, and its summary line never printed either way to say
so. `FramePacing` already had the fix; the other seven now share it.

**Counting `Add*` return values counts what was *queued*.** `HookManager.AddPre` /
`AddPost` only append a delegate to a dictionary and return `true` unless the
*signature* is wrong. The detour is created later, in `HookManager.Commit()`, which
since `0027` catches per function and prints `[Mods] could not hook …` rather than
throwing — which is exactly what makes a partial install possible. So the port could
print `boundary 3/3 DrawOTag + 3/3 VSync` while no boundary existed, latch itself
done, and never try again — the uncapped-and-8×-speed failure this whole subsection
is about, reported as a healthy session. `patches/recompone/0028` adds
`HookManager.IsCommitted`, and every summary line here is now read back from it
after the commit.

**A pre and a post on one function share one detour**, so those two land or fail
together and the read-back cannot separate them; what it is for is a patch whose
sites are *different functions* — `AnimSmoothing`'s five, `FramePacing`'s
per-overlay roles — and every claim printed to the console.

`patches/HookAttach.cs` holds both halves: `OnOverlayLoad(label, attach, hint)` is
the retry latch set from the pass's own success and capped at `MaxTries` (3 — a
miss is nearly always a miss for good, since every overlay is registered before the
first load, so this only has to cover a transient), and `Installed(target)` is the
read-back. A pass must therefore be safe to re-enter and must add only what it is
still missing, which is why each patch keeps a set or a flag per role.

Measured: with `KF2_FPS_GATE=80037C0C+DEADBEEF`, `pacing` reports `1/2 stage(s)`,
retries on each of the next two overlay loads without re-adding the role it already
has, and then says `giving up on the rest; what is hooked is hooked. See "Any frame
rate" in docs/PATCHES_AND_MODS.md.` With nothing broken, all eleven patches report
fully attached and `walk` is unchanged — 2928 units/2 s at 20 fps and 2821 at 144.

#### Half a pair cannot be re-enabled from the settings pane

The read-back also closes a hole the safety disables had. `FrameSmoothing`,
`ObjectSmoothing` and `SpriteAnim` each set `Enabled = false` when only half their
pair attached — *"half a pair would leave interpolated positions in the table for
the AI to find, which is the one outcome this must never have"* — but never recorded
the decision, and `FrameSmoothingPage` calls `SetEnabled(true)` straight from the
Video checkbox. One click re-enabled a pre with no post, which writes lerped
positions into the entity, object and effects tables every frame with nothing to put
them back, so the next tick's AI, collision and any save read interpolated
coordinates. Each now latches `_paired` in `Attach` and `SetEnabled` is
`Enabled = on && _paired`, so the disable cannot be talked out of.

`AnimSmoothing` had no such disable at all — its `Pair` return was only ever counted
into one `Console.WriteLine` — and it is the patch where a half pair is worst,
because four of its five sites keep state a post balances: `AfterClock` without
`BeforeClock` writes the carried weight through a pointer left over from an earlier
frame's stack, `BeforeSubmit` without `AfterSubmit` climbs `_depth` past 1 so every
later submit is rejected, `BeforeArm` without `AfterArm` latches `_inArm` and keys
the HUD builder's `MoApply` in under the arm's id. It is now all-or-nothing on all
five, and reports which site failed.

`SetEnabled` also re-primes. While off, `Before` returns before sampling, so
`Prev`/`Cur`/`Live`/`Carry` freeze at whatever they held and only an overlay load
clears them — a player who unticks the box, plays a minute in the same area and
re-ticks it got one frame in which every object was drawn where it stood a minute
ago.

#### `LoopPacing`'s degradation was the opposite of what it claimed

Its missing-marker diagnostic said losing the marker means *"every frame reads as
the main loop's … so this degrades to 'no fix'"*. It is the other way round:
`MainLoopStage` is what *sets* `_mainLoopSeen`, so with it unhooked the flag is
permanently false, `FrameMinMs` reads `mainLoop == false` on **every** frame, and
the class caps the whole game at `InterfaceHz` while `if (mainLoop) Reprime()` never
fires — which latches `_carriable` and starts lerping stage 13's view in the main
loop. Anyone chasing an unexplained 60 fps ceiling would have been steered away from
the cause. `FrameMinMs` now reads `_mainLoopSeen || !_markerHooked`, so an unhooked
marker really is the stand-down the line claimed, and the message says so.

`MenuPacing` and `LoadPacing` had the same shape as `return`s rather than as
inverted comments. `MenuPacing`'s repeat gate and blink stepper are separate
findings on separate functions, and the blink was handled independently in one
direction but not the other — a resolve miss on the repeat gate returned before the
blink pair was ever registered. `LoadPacing`'s `VSync`-thunk miss returned *after*
up to four hooks were registered on the two disc waits, leaving them uncommitted in
`HookManager`'s dictionary — installed only if some other patch happened to call
`Commit` later in the same event — and skipping the one line the class prints, in
the case where it most needs to say what it did. Both now attach each role
independently, always reach `Commit`, and name each role in the summary:

```
[KF2] menu pacing: on, repeat every 100 ms, blink capped at 60 Hz
[KF2] load pacing: on, 2/2 disc wait(s), VSync held to 60 Hz, counting steps
[KF2] anim: timeline, hooked 5 of 5 site(s) (clip clock, arm)
```

### Three things held the port at 30, and only one of them was the game's

| # | gate | where |
|---|---|---|
| 1 | **the game's own frame gate**, `func_80017880` — spins on the vblank credit at `0x801B6CA8` until it reaches **2**, then zeroes it. Called by stage 13 as its last act. | `GAME.EXE`; see "The loop's own rate gate" in [GAME_INTERNALS.md](GAME_INTERNALS.md) |
| 2 | **`FrameClock`**, a hard-coded 60 Hz applied per `VSync` *call* inside `Runtime.PresentFrame` | `patches/recompone/0025` makes it settable |
| 3 | **`FramePacing.Floor()`**, the port's own deadline at the frame boundary | `patches/FramePacing.cs` |

Gate 1 is the one that was missing from the earlier write-up, and it is decisive:
since `0021` advances the emulated vblank on a wall-clock 60 Hz grid, that spin
paces the port to exactly 30 fps whatever the host does. **No rate above 30 is
reachable while it runs** — which means the odd/even stage gate the port shipped
before was skipping stages on frames that were still 33 ms apart.

`patches/FramePacing.cs` therefore hooks it with a `pre` that returns `false`,
writing `0` to `0x801B6CA8` itself because the function that would have zeroed it
did not run. Only `GAME.EXE`'s copy is hooked — `OPEN.EXE` and `END.EXE` link the
same routine at their own addresses, but the title and the ending are CD-bound at
7–15 fps and have no world to tick.

**That hook used to return `true` at 30 and below**, on the reasoning that there
the game paced itself exactly as it does on hardware and the port added nothing.
That stopped being true the moment the tick rate became a number of its own: the
gate does not only pace, it also decides how often the world advances, and it
knows one answer for both — 30. Left running at the 20 fps default it would pin
the world back to 30 Hz and make `LogicHz` a lie exactly where it matters most.
So **it is now skipped at every rate**, and two consequences follow:

* **The port's most-tested configuration is gone.** There is no longer a setting
  at which none of this class does anything; `Floor()` is the pacer always.
* **A rendered frame carries exactly one `VSync` call at every rate** — the
  presenter's — because the gate's spin calls were the second one. The frame
  boundary is therefore the same shape at 20 fps as at 144, which is the simpler
  of the two cases it used to have to handle. `mods/framestats` reading `2` on
  calls/frame now means the gate is back.

`0x801B6CAC` and the vblank callback `func_80017850` are untouched: only the spin
is skipped, so the CD timeout riding on that counter is unaffected.

`LibEtc`'s vblank grid is deliberately **left at 60 Hz**. The music sequencer, the
root-counter events and the game's own `0x801B6CA8` all hang off it, so raising it
would speed the audio up; present rate and vblank rate are independent, which is
exactly what `0021` set up. `FrameClock` is handed a permissive ceiling
(`2 × target`, never below 60) rather than the target, because it paces per
`VSync` call and a frame can carry more than one — a per-call throttle cannot
express a frame rate. The floor, which sits at `DrawOTag` and therefore knows
where a frame ends, is the only real pacer.

### The logic clock

The loop runs at whatever the floor allows, so the world would advance a fixed
amount that many times a second. A wall-clock accumulator ticks at `LogicHz` — 20
by default, 30 the other way — and the main-loop stages holding per-tick state run
only on a frame where it ticked. It is advanced by elapsed *time*, not by
`1/target` per frame, so a host that misses the target runs the world at `LogicHz`
anyway — the difference between "the picture stutters" and "the game plays in slow
motion".

**It runs in every configuration now**, not only above the tick rate, because
skipping the game's gate left it as the only thing holding the world down:

* At the render rate *equal* to the tick rate it ticks on every frame and nothing
  is skipped, because `Floor()` guarantees a frame is at least `1000/LogicHz` ms
  and the credit always reaches 1. Measured at `KF2_FPS=20`: 20.00 ticks/s.
* **Uncapped no longer runs the game fast.** `KF2_FPS=off` draws flat out (60,
  held there by `FrameClock`'s own ceiling) and still measures 19.99 ticks/s. The
  settings label said "runs too fast" and no longer should.
* Below the tick rate the world cannot catch up: a stage can be skipped but never
  run twice, so it ticks once per frame and the whole game plays slow. Measured at
  `KF2_FPS=20 KF2_TICKRATE=30`: 20.00 ticks/s, not 30. That is a diagnostic
  configuration, and the settings note says so.

Five things are gated:

```
3  func_8002A550   pad read, turn, walk, angle fold, the death counter at
                   0x8019951A, the poison tick, the buff timers at
                   0x80199472..0x80199482, and 0x80199488, the global frame
                   counter it bumps last
4  func_80040348   the 200-record entity table at 0x8016C544. Its AI runs one
                   entity in four off 0x80175908 & 3, which stays right for free
                   once the stage itself is gated
5  func_80046A60   128 effect/projectile slots at 0x8019CC6C, each with a lifetime
                   at rec+0x0E decremented once per call. Ungated, every spell and
                   effect expires at the render rate
6  func_8004910C   the area module's own per-frame entry -- slot 1 of the module
                   header, reached as *(u32*)(*(u32*)0x8017E068 + 4)
13 func_80033FBC   the fade state machine stage 13 calls, not stage 13 itself
```

`KF2_FPS_GATE` replaces that set, and `KF2_TICKRATE` sets the rate they run at.

**"Can it draw" is the test each of them had to pass**, and it is the reason the
list is what it is rather than "everything with a counter in it". A stage that
submits primitives cannot be skipped — at 120 fps three frames in four would be
missing whatever it drew. The check is static, against the emitted C#: walk the
function's call subtree and look for `DrawOTag`, `VSync`, `PutDispEnv` or
`PutDrawEnv`. What that found:

* **Stage 6** dispatches indirectly, so the target has to be read out of the
  module image rather than the call graph: slot 1 of the 32-word header, which is
  `FDAT.T` at the overlay's own `offset`. Six of the nine modules leave it an empty
  `jr $ra` (`fdat02`, `05`, `08`, `17`, `23`, `32`); `fdat11`, `fdat14` and
  `fdat20` use it for proximity and trigger logic — `fdat11`'s reads the player's
  position, calls the angle helpers `func_80015394`/`func_80015328` and writes
  state bytes. **No SDK entry point in any of the nine subtrees**, so it is gated.
* **The fade**, `func_80033FBC`, is a three-function subtree that cannot draw,
  which is why it can be gated even though its caller is the renderer. It is a
  four-state machine on the byte at `0x80192D42`: brightness `0x80192D44` steps
  `+0x14` a call until it reaches `0x64`, a hold counter at `0x80192D43` counts
  down, then brightness steps `-0x14` (written as `+0xEC`) back to zero. Ungated
  that whole sequence ran at the render rate and an area fade was four times
  quicker at 120 fps than on hardware.

**Stage 2 — the world's moving props, and why "can it draw" nearly kept it out.**
Doors, the drawbridge, the minecart and the spinning crystals all live in
`func_80037C0C`, main-loop stage 2, and until this was gated they moved at the
render rate. It walks the **object table at `0x80177714`** — 396 slots of `0x44`,
a slot free when the type byte at `rec+0x4` is `0xFF` — publishing each record to
`0x8017E04C` and its `0x18`-stride definition (indexed by the `u16` at `rec+0x6`,
in the table at `0x80175914`) to `0x8017E048`, then dispatching on that same type
byte through a **224-entry jump table at `0x8001191C`**, collapsing to thirty
distinct arms plus an indirect arm into the area module's own handler. Counting
its writes through the record base gives the census's three fields exactly:
`rec+0x18` (the position VECTOR's Y lane, 7 sites), `rec+0x24` (6) and `rec+0x40`
(20), alongside `rec+0x14`/`rec+0x1C` for X and Z and the state word at
`rec+0x08` — 43 sites, the busiest field in the function and the door/lift state
machine itself.

It was excluded for years on the reading that "all four SDK entry points are in
its 268-function subtree, so it presents". That is true of *reachability* and
false of what the rule protects against. Enumerate every function in the subtree
that calls a submitting or presenting entry point **directly** and there is
exactly one, reached by exactly one edge:

    func_80037C0C -> func_80037B5C -> func_800342D8 -> func_8002E0FC -> DrawOTag

`func_80037B5C` is a self-driving fade loop — it steps a tint from `a1` to `a2` by
`a3` and calls the tint drawer, stages 11, 12, 8 and **stage 13 itself** for each
step. It is entered from one arm of the state machine (an in-bounds trigger), and
the indirect area-module arms get to the renderer the same way, through the
message-box and cutscene loops `func_80047000`, `func_80048208` and
`func_8004831C`. Every one of those is an **extra** render inside the stage, never
the frame's own: the main loop still runs stage 13 afterwards. That is the same
exception stage 3 already carried, and a strictly narrower one — stage 3 has nine
such paths including a whole blocking menu session. So stage 2 is gated, with the
reason recorded in `check_gate.py`'s `KNOWN`, and **the cost is that entering a
fade or a cutscene can be deferred by up to one tick (50 ms)**. Skipping cannot
cut one in half: once entered, the stage is on the stack and drives its own
frames.

Measured with `rate_census.py --run --scenario idle --fps 20 144`, standing still
in area 1, the four records that move:

| field | before (20 -> 144) | after (20 -> 144) |
|---|---|---|
| object table `+0x18` | 13.0/s -> 49.8/s, ratio 3.84 | 13.7/s -> 17.7/s, ratio 1.29 |
| object table `+0x24` | 13.0/s -> 52.4/s, ratio 4.05 | 13.7/s -> 17.7/s, ratio 1.29 |

For scale, in the same pair of runs the already-gated animated-texture phase reads
16.8/s -> 18.4/s (ratio 1.10) and the global frame counter `0x80199488` reads
15.5/s -> 18.1/s (ratio 1.17). The props now alias the way the things that were
already right do. **The picture has not been looked at** — whether a fade or a
cutscene now starts visibly late, and whether a 20 Hz prop against a 144 fps
picture wants `KF2_SMOOTH_OBJECTS=1`, are both eye questions.

**What this still does not cover, recorded rather than discovered later.**

* **`rec+0x40` on two other slots is a different defect and is still open.** It
  survived the gate at ratio 3.69 (14.4/s -> 53.4/s), and gating `func_800331B4`
  as an experiment dropped it to 17.6/s, which is the attribution: the writer is
  **stage 13's own object pass**, not stage 2. Reading it, `rec+0x40` is a
  retrigger deadline in **vblank** units against the free-running count at
  `0x801B6CAC`, re-armed as `vbl + 6 * (u16 at rec+0x3E)`, and when it expires the
  routine computes a distance-attenuated volume against the player position at
  `0x801994EC` and calls the sound player `func_80014158`. So it is a per-object
  **ambient sound** — and with a small interval it retriggers once per rendered
  frame up to the 60 Hz vblank ceiling, i.e. 20/s at the default and 60/s at 144.
  It cannot be gated: `func_800331B4` walks the table and draws the models in the
  same loop, so no whole-function hook separates the two. **Not heard by ear**;
  the numbers above are all that is known.

**Animated textures — the sixth gated function, `func_8002DC78`.** The animated
water at the start, the main-hall fire and the creatures' scrolling skins are one
system: **eight slots at `0x80192D58`** (stride `0x18`), each holding a scroll
phase at `rec+0x4`, a per-slot advance rate at `rec+0x3`, a wrap at `rec+0xC`, and
a source-texture pointer at `rec+0x10`. `func_8002DC78` walks all eight every call —
it advances each phase, then re-uploads the scrolled region of the source texture
to VRAM through `func_80060624` (a `LoadImage`-style GPU transfer). It is called
once, straight from **stage 13** (the renderer, `func_800342D8`, at `0x800346..`),
so ungated it ran once per *rendered* frame: the scroll advanced at the render
rate — measured `rec+0x4` changing on **100 % of frames at 120 fps**, six times too
fast against the 20 Hz world (and the reported "runs high with the framerate":
faster the higher the frame rate). Its whole subtree is the VRAM upload — no
`DrawOTag`/`VSync`/`PutDispEnv`/`PutDrawEnv` — so it passes the same "can it draw"
test as the fade stepper `func_80033FBC` and is added to `FramePacing.DefaultGate`
alongside it. On a non-tick frame it is skipped: the phase holds and VRAM keeps the
last tick's frame, so the texture animates at the tick rate whatever the port
draws (measured `rec+0x4` back to **17 % of frames at 120 fps**, i.e. 20 Hz). The
rate is one more address in the gate; the picture of it is `FluidSmoothing`,
below.

**The water still steps at the tick.** The rate per second is right, and a 60
or 144 fps window still shows water jumping a texel every 50 ms, because VRAM
only changes on a tick. Re-uploading at the render rate would run the integer
scroll *faster*, not smoother, and the frame viewer measured the tick-rate
upload as 0.76 ms on the next primitive before `0054` stopped the scaled blit.
Offsetting the packet's 8-bit V is not enough either: both rasterizers snap UV to a whole texel, so a
fractional leftover rounds away. `patches/FluidSmoothing.cs` publishes the dest
RECT and leftover phase (`(1 - LogicPhase) × delta` added to V, the same clock
the view is drawn at) to the prim shaders (`0053`), which blend the two wrap-rows
the upload itself wraps between. Matching is on the dest RECT, so every assembler is
covered and Fast geometry is not required. GL only; the software rasterizer
keeps the last upload. On with the rest of the smoothing tick;
`KF2_SMOOTH_FLUID=0` is the comparison. **The picture has not been looked at.**
Its cost is not measurable: facing the water in `fdat02`, work 3.09-3.13 ms on
against 3.00-3.12 off, and the same GPU time. What water on screen did cost was
the batch submits its semi-transparency forces, see "Water on screen cost 5 ms a
frame" in `docs/DEVELOPMENT.md`.

* **The jitter accumulator at `0x8006E608`** is in stage 13's *own body*
  (`func_800342D8`), not in a callee, so no hook can reach it — `HookManager` only
  detours whole functions and stage 13 must draw. It sums `func_80015374()` and
  decays by an eighth a call to drive the screen shake, so above the tick rate the
  shake settles faster and smaller. A quirk of amplitude, not a timer.

### The view has to be carried between ticks

A camera moving at the tick rate but presented several times as often is not
merely no better than drawing at the tick rate, it is worse: the picture updates
and the view does not. **Dropping the tick to 20 makes this more load-bearing, not
less** — a tick is now 50 ms rather than 33, so the camera stands still for half
again as long between them. `patches/FrameSmoothing.cs`
is one `pre`/`post` pair around **stage 8** (`func_80025A1C`), which is the whole
of "build the render camera from the player state" and the only thing between that
state and the picture — so the carried view lives for exactly one function call
and cannot accumulate, cannot reach the collision code and cannot reach a save.

**It interpolates, it does not extrapolate — and that is what stopped the bounce.**
The first version carried the view *forward* by last tick's velocity (`angle +
turnVel × frac`). That is smooth only while the velocity holds, and King's Field
damps a turn and stops dead at a wall, so the next tick's real angle was routinely
*less* than the one predicted and the view snapped back to it — reported as *"the
camera bounces back to a position it would have travelled in 20 Hz"*, every time a
turn eased off. It now keeps the view the game produced at the previous tick and at
this one and draws `lerp(prev, cur, frac)`, which can never reach a position the
game did not produce, so nothing overshoots and nothing snaps. The cost is a tick
of latency — the picture trails input by up to 50 ms — but the input is sampled at
the tick rate anyway, so that is a delay of the *display*, not of the response.
**The mouse is the exception**: it asks for a displacement the game applies
exactly, so the picture shows it before the tick lands rather than a tick after —
see "The mouse leads the tick" in `docs/INPUT.md`.

* **Yaw and pitch** are interpolated between the composed view angles at
  `0x80199504` / `0x80199506`, re-sampled on every frame the world advanced on.
  `frac` is `FramePacing.LogicPhase`, continuous across a tick boundary, so the
  camera does not jump on the frames where the world did advance. Yaw is 12-bit and
  wraps, so the interpolation takes the shortest way round.
* **Position** is interpolated between the player position at `0x801994EC`/`F0`/`F4`
  that `func_80028080` writes after the collision test, so walking a wall
  interpolates between two positions that already slid along it — no overshoot into
  it, and no snap back. A step past 1024 units on an axis is a warp rather than a
  walk and is left alone, the way `ObjectSmoothing` guards a placement.

**Both default to on** — the shipped picture is 60 fps against a 20 Hz world.
The boundary bug that once pinned `LogicPhase` to 0 (a counted boundary was a whole tick
wide, so the credit went `0 → 1 → tick → 0` and never sat between) is long fixed;
the probe now reads e.g. `241/241 frames carried, mean phase 0.50 tick, yaw 17.7 u`
at 120 fps. The picture was checked by eye after the switch to interpolation and
reported *"incredible"*, and that is now the first-run picture.

#### Four things review found in it

All four are in `patches/FrameSmoothing.cs`, and three of them are the same shape:
a guard that reads the wrong predicate, so the patch is silently wrong in a
configuration nobody had run the probe in.

* **It ran at the tick rate, where it can only add latency.** The pre-hook was
  gated on `FramePacing.Gating`, which is true at *every* rate — the frame gate is
  skipped whatever the setting — where the test it wants is
  `FramePacing.Extrapolating`, "a frame can land part-way between two ticks".
  `LoopPacing` and the settings page were already using it. At the port's own 20
  fps default every frame ticks, `LogicPhase` is ~0, and `lerp(prev, cur, 0)` is
  `prev`: with `KF2_SMOOTH=1` the renderer was handed **last** tick's camera on
  every single frame — a permanent 50 ms of view latency bought with no smoothing
  at all, and silent, because the greyed-out checkbox reads as "this is not doing
  anything". Measured before: `41 of 41 frames carried` a report at
  `KF2_FPS=20`. After: `0 of 41 … 41 off or at the tick rate, not extrapolating`.
* **Re-enabling from the Video pane carried from stale samples.** `_prev`/`_cur`
  are only refreshed on a tick frame *while enabled*, and `_carriable` was cleared
  only on an overlay load — so unticking the box, turning 180° and walking down a
  corridor, then ticking it back on, wrote `lerp(stale_prev, stale_cur, phase)`
  into the live camera words on the very next frame. `SetEnabled` primes again on
  the off→on edge, which is what `AnimSmoothing.SetCarry` already did with
  `_slots.Clear()`.
* **The carried angles were written in a representation the game does not use.**
  The composer (`func_8002A550`) sums four zero-extended halfwords and stores the
  low sixteen bits, so a negative pitch is the s16 the field is typed as — -700 is
  `0xFD44`. `S12` recovered that correctly on the way in, but the write was
  `(S12(prev) + step) & 0xFFF`, which puts `0x0D44` back: +3396 to any consumer
  that reads the word as signed rather than indexing a sin/cos table with
  `& 0xFFF`. Stage 8 is not the only consumer — the two stack blocks it fills go
  on to stage 9, the 3D sound listener, and to stage 13. Both angles are now
  stepped from the game's own previous-tick word in the game's own 16-bit domain
  (`(ushort)(_prevYaw + yawStep)`), which is bit-exact at phase 0 and agrees with
  `cur` mod 4096 at phase 1.
* **The probe could not report the thing it was written to report.** `_skipped`
  had one writer, the "nothing moved between the two ticks" case, and it always
  fired together with `_skipStill` — so the `_carried == 0` branch printed the same
  number twice and could never say anything else. Frames dropped for being off, at
  the tick rate, or not yet primed were counted **nowhere**, so when they were all
  the frames `total` was 0 and `Report` returned silently: the exact configuration
  this probe was written to diagnose printed nothing at all. Three buckets now, and
  no early return on `total == 0`.

#### The position half is knowingly inconsistent

The class comment claimed the stage-8 isolation covers everything it carries. It
covers the **angles**. `docs/GAME_INTERNALS.md` ("Stage 8 is the render camera, and
it is the only copy") records the other half: two of stage 13's own callees read
the player position triple *directly*, after stage 8 has run and after
`FrameSmoothing.After` has put the raw values back — `func_80032400`, the
first-person arm, reads `0x801994EC` and `0x801994F4`, and `func_800331B4`, the
world and object walks, reads all three. So with `KF2_SMOOTH_POS=1` the eye is
carried and the origin those two place their geometry against is not: on a non-tick
frame the arm, the torches and the creatures shear against the architecture by
exactly the distance the carry moved the eye, worst just before a tick lands. That
is the constant-offset failure `ObjectSmoothing` was written to avoid, in a
different place. It is why the position used to be a separate switch and off by
default; it now ships inside the one smoothing tick. Closing it means carrying
those two readers too, inside the stage
`ObjectSmoothing` already brackets. The comment now says so instead of asserting a
guarantee the code does not have.

#### The head bob

**Mechanism measured; the picture has not been judged by eye.** Stage 8 builds the
eye height as `Y + s16 0x80199548 + s16 0x8019954C - 0x640`, and until this the
carry took the angles and `Y` but not the two offsets, so with everything else
moving at the render rate the bob stepped at 20 Hz. Reported from play as "jarring".

The bob is written on the tick by `func_80028560`: a phase at `0x8019954A` advances
by the step at `0x80199542` (the horizontal speed, `SquareRoot0` of the velocity in
`func_800290D4`; 200 walking), wrapped to 12 bits, and the bob is
`0.75 * |rsin(phase) >> 5|`, **0 to 96 units** — a rectified sine, so the view
bounces off a sharp V at each footfall by design. It is updated only while the byte
at `0x801994E4` (a landing state `func_80028560` sets to `0x10`/`0x40`) is zero, and
forced to 0 while **`0x801994E0` is zero — the last of the six toggles on the game's
own options screen** (`func_8001BB7C`; `func_80025CCC` defaults all six to 1). Test
save 2 has it off, which is why an agent run shows no bob at all. Stopping does not
snap the view: the step ramps down over seven ticks and the phase freezes, leaving
the bob wherever it stood.

Nothing on the render side reads either word after stage 8 — the other readers
(`func_80023ECC`, `func_80026EA4`, `func_800271D0`, `func_8002A550`) are all under
the tick stages — so they are carried inside the same pre/post pair as the angles,
with the same isolation, and restored in `After`. A plain lerp is right: the value
itself never wraps (the phase does, and is not touched).

Measured with the option forced on, walking in area 1 at 144 fps: the bob runs
0-96 in steps of up to ~30 units a tick, `KF2_SMOOTH_PROBE=1` reads `bob carried on
287 (247 between the ticks' values)`, and pacing holds at 144.0 fps drawn and 20.0
ticks/s. The landing offset was never non-zero on that run and is carried on trust.
Whether the V at the bottom still reads as harsh once it is no longer stepping is
the question left for the eye.

### The camera is not the only thing that moves

Reported after the tick rate became a setting, playing at 60 fps against the 20 Hz
world with frame smoothing on: *"the enemies move at the correct speed, but they
are animated at a visibly lower framerate, while the HUD renders at 60 — and the
player's arm renders at 20 as well."* Every part of that is the design working as
built, and two thirds of it are fixable.

Smoothing the camera covers more of the picture than it sounds like it does,
because most of the picture is architecture that never moves: reproject a static
wall through a camera that moved and it is smooth. The HUD is rebuilt by stage 13
every rendered frame — its digits and gauge widths from the live HP/MP, its
orientation from the camera — so it is exempt by construction. What is left over is anything carrying a position of its own —
which still advances once a tick, and which now steps against a world sliding
smoothly past it. That contrast makes the step **more** visible than it is with
nothing smoothed at all, which is why this is the other half of frame smoothing
rather than an extra beside it.

`patches/ObjectSmoothing.cs` (`KF2_SMOOTH_OBJECTS=1`) is the same shape as
`FrameSmoothing` — a `pre` that writes, a `post` that puts back exactly what was
there — around **stage 13** (`func_800342D8`) rather than stage 8, because it is
the renderer that reads these and not the camera builder. Stage 13 writes nothing
but the display list, so the interpolated positions are gone before the next tick's
AI, a save or a proximity trigger can see them.

**There are two tables, because the renderer walks two.** `func_800331B4` loops
the **entity table** `0x8016C544` (200 records of `0x7C`, drawn when
`u8[+0x9] == 1`, position a `VECTOR` at `+0x2C`, rotation three `s16` at `+0x40`)
for **creatures/enemies**, then loops the **object table** `0x80177714` (396 slots
of `0x44`, `VECTOR` at `+0x14`, drawn when `u16[+0x6] != 0xFF`) for static props
and sprites — both the constants `patches/AgentServer.cs` already reads for
`nearby` (`entities` and `objects`), though **neither of `AgentServer`'s liveness
tests is the renderer's**: it uses the owning stage's `+0x0` and `+0x4`, which is
right for "what exists" and wrong for "what is drawn".

For a while this carried only the object table, on the belief — from a
`KF2_DRAWCENSUS=2` reading of `func_80032588`'s `a2` — that "the renderer reads the
object table and not the entity record." That reading was taken with props on
screen and **no creatures near**, so it saw only the second loop. The entity record
is a copy *and* a source: stage 4 copies the object position into `rec+0x2C`, but
the first loop then draws creatures from that copy, plus its own rotation at
`rec+0x40`. Smoothing the object table alone therefore left every enemy
stepping in **both** position and facing — the reported jitter — so this carries
**both** tables, and the entity rotation on top. See "What in the renderer draws
what" in [GAME_INTERNALS.md](GAME_INTERNALS.md) for how that was established.

**It carries four tables now, not two, and it is a list rather than four copies of
the code.** The renderer draws from four (the inventory is in "What in the
renderer draws what" in [GAME_INTERNALS.md](GAME_INTERNALS.md)); this carried two,
and *both* omissions reached a player as "the animation runs at a low frame rate"
before anyone read `func_800331B4` to the end. The two that were missing are
**stage 5's table at `0x8019CC6C`** (128 x `0x48`, position `+0x14`, rotation
`+0x24` — the same layout as the object table, and gated, so it stepped at the
tick rate with nothing carrying it) and the **billboard sprites at `0x80195174`**
(128 x `0x18`, position `+0x8`, no rotation — already render-rate, so carrying
them is a no-op and harmless). `ObjectSmoothing.Tables` is now a `TableSpec[]` and
one routine walks it, so the next table is a row.

**The object table's emptiness test was the owning stage's, not the renderer's.**
Stage 2 steps a slot when the byte at `+0x4` is not `0xFF`; `func_800331B4` draws
it when the `u16` at `+0x6` is not `0xFF`. This used stage 2's, so any slot that
is drawn but not stepped by stage 2 was treated as free and never carried. What is
being interpolated is what is *drawn*, so the drawing test is the right one — and
a slot carried but not drawn costs a write and a restore and nothing else.

**The object table has a rotation lane too, and missing it left doors animating at
20 Hz.** This section used to say the object table had no rotation at all, on the
strength of the same draw census reading only `a2`. It has one — three `s16` at
`rec+0x24`/`+0x26`/`+0x28`, with the identical `0x800` yaw bias, built into the
`a3` triple by the object loop of `func_800331B4`. Carrying position alone left
anything that *turns* stepping at the tick rate against a position that glided,
which is what a door closing looks like at a low frame rate while its speed is
right. It is now carried the same way the entity lanes are: shortest way round a
4096-unit turn, the bits above the mask preserved, and restored in the post-hook.
Two things about it differ from the position path on purpose — **rotation is not a
rider on position**, so a slot whose origin never moves (a door swinging on its
hinge) is still carried, where the old `dx==dy==dz==0` skip dropped it; and **the
1024-unit placement guard is the position's alone**, since `DeltaAngle` already
takes the short way round, so a re-placed facing costs at worst half a turn of
sweep and cannot veto the slot. Measured at 144 fps, standing in a quiet area:
`289/289 frames carried, 4.0 object(s) each, 4.0 turning, biggest angle step 128 u`
and no `LEAKED` — 128 is `0x80`, exactly what the arm at `0x80038BA8` adds to
`rec+0x26` each tick, i.e. 1/32 of a turn. **Not looked at by eye.**

The entity rotation is interpolated the shortest way round a **4096-unit turn** (the
`0x800` yaw bias the renderer adds is half of it). The raw lanes are not confined to
one turn — the probe measures signed/accumulated values above `0xFFF` and just under
`0x10000` — so the carry works modulo 4096, the part the GTE reads, and leaves the
bits above it untouched. Measured at 60 fps in an area: `241/241 frames carried, 3.0
creature(s) each`, no leak, alongside the object pass.

Two things about it are worth stating:

* **It interpolates, on the same clock as the view — and the clock is the whole
  point.** It interpolated, was switched to extrapolating, and now interpolates
  again. Interpolating was right on its own terms — nothing in this table is steered
  by the player, so a tick of latency is free, and walking between two positions the
  game produced cannot overshoot. It was abandoned only because the *camera* then
  extrapolated: `FrameSmoothing` drew the view forward to `t + frac` while
  interpolating drew an object at `t - 1 + frac`, a whole tick apart — 50 ms at the
  default rate — and that constant offset did not read as latency, it read as **the
  objects moving more slowly than everything else** (*"the enemies still move
  visibly slower than the compass"*). The camera now interpolates too, so the two
  are back at the same instant, and interpolation is the better tool wherever it can
  be afforded: no bounce-back on a stop or a turn, which is exactly what forward
  extrapolation gave. **Two smoothers must agree about what time it is** — that is
  the rule worth carrying to whatever gets smoothed next.
* **A step over 1024 units on any axis is a placement, and is left alone.**
  Without it an object spawned, respawned, moved by a script, or simply placed
  when the area finished loading is swept a whole area's width over one tick. (It
  is needed for interpolation just as it was for extrapolation: prev and cur can
  straddle a placement either way.) The threshold comes from measurement, not
  taste: real motion peaked at **37 units a tick**, the player — the fastest thing
  in the game — covers 1817 units in 2 s at 20 Hz and so about **45 a tick**, and
  one window caught a **233,472-unit** step at area load. 1024 sits twenty times
  above anything that walks and two hundred times below the placement it has to
  catch.
* **It was raised to 8192 and made sticky, and that was reverted. The reasoning is
  worth keeping.** Play reported the final boss and the piranhas freaking out
  during an attack, and the argument was that a part whose step sits near the
  threshold is carried on the tick it comes in under and held on the tick it goes
  over, gliding and stopping twenty times a second while the parts either side of
  it keep gliding — the creature tearing itself apart. A bare threshold being a
  cliff, the decision was also made sticky, a slot already carried getting
  `GlidingFactor` × the threshold before it is called a placement.

  **The mechanism was impossible.** A creature is *one record* in the entity table
  — one position, one rotation triple (`0x8016C544`, 200 × `0x7C`, position
  `+0x2C`, rotation `+0x40`) — so the finest thing this patch can act on is the
  whole creature. It can make a boss judder as a body; it cannot shear one limb
  against another, and a limb-relative defect is the MO pose, which is
  `AnimSmoothing`'s clip clock. The boss was never confirmed to improve at 8192,
  so nothing verified was gained.

  **And the raise broke projectiles.** Play reported fireballs stuttering and
  appearing where they had not been, at anything above 1024. The effects table
  (`0x8019CC6C`, 128 × `0x48`) recycles its slots constantly, and a slot freed and
  refilled inside one tick is never observed free: `Prev` holds the dead
  projectile and `Cur` the new one. 1024 refuses that delta as a placement; 8192
  carries it, and the new fireball is drawn sliding in from wherever the old one
  died. Stickiness makes it worse again, since a recycled slot inherits the
  previous occupant's `Gliding` along with its position. That is a **wrong**
  position, not a rough one, so an unverified fix bought a verified regression and
  the default went back.
* **The threshold is per table, and that is the compromise.** Play, with the guard
  switchable: on `strict` the boss no longer freaks out and projectiles are
  perfect, but a boss moving fast still looks as though *its head snaps into the
  next frame of the animation*; on the raised modes the animation gains visible
  in-between frames and projectiles break. Both halves have the same cause. A
  creature is drawn from **two** smoothers — its root from `ObjectSmoothing`, its
  pose from `AnimSmoothing` — and when the guard refuses a fast creature's
  position, the root steps at the tick rate while the vertices go on morphing at
  the frame rate. The pose then slides ahead of the body, which is what the head
  snapping is; the raised modes "add in-between frames" mainly by giving the pose
  a root that moves with it. Meanwhile the projectile tables recycle slots
  constantly and are wrong at anything above 1024. The tables want opposite
  answers, so `TableSpec.Fast` scopes the raise to the **entity table** and every
  other table keeps 1024 whatever the mode is set to.
* **When a root is held, the pose is held with it.** Scoping is not enough on its
  own: a creature past even the raised cap brings the head snapping straight back.
  `ObjectSmoothing` publishes the position addresses it refused this tick and
  `AnimSmoothing` refuses those slots' pose carries for the same tick, so such a
  creature degrades to a coherent tick-rate creature rather than an incoherent
  smooth one. The two patches need no knowledge of each other's tables to do it:
  `func_80032588`'s `a2` — what `AnimSmoothing` already keys its per-creature
  state on — *is* `base + slot*stride + PosOff`, the address the carry writes
  through. The coupling is inert when `ObjectSmoothing` is off, since nothing is
  carrying any root then and holding every pose would disable pose smoothing
  rather than keep two smoothers in step. `KF2_SMOOTH_ANIM_PROBE=1` counts it as
  `refused (root held)`.
* **The default is `continuous`**, which is the mode play reported as visibly
  smoothest on a creature. What made the raise unshippable was projectiles, and
  the raise no longer reaches them. `sticky` is the more conservative choice if a
  creature is ever seen sliding in on spawn.
* **The lasting finding is that this threshold was doing two jobs.** Rejecting
  slot reuse and teleports wants it tight; admitting fast honest motion wants it
  loose. One global constant cannot serve both — the boss argument and the
  fireball report are the two ends of exactly that — and both numbers were derived
  in a quiet corridor from things that walk, which never bounded either job. The
  principled fix is to take the first job away from the threshold entirely by
  keying the sample on the slot's **identity** (the object table's definition
  index at `+0x6`, the entity table's byte at `+0x0`) and dropping the previous
  sample whenever it changes, so reuse is caught regardless of speed. Failing
  that, the threshold belongs per `TableSpec` rather than global: creatures walk,
  projectiles do not.
* **The symptom does not reproduce at 1024 any more**, which closes the argument.
  1024 is the value that was in place when the boss was first reported spazzing,
  so if the guard were the variable the revert would have brought it back. It did
  not. The only other behavioural change since that report is `AnimSmoothing`'s
  segment overrun becoming a **refusal for the whole tick** rather than a clamp to
  the segment's start — which is the pose, the layer that can actually move one
  limb against another. Not yet confirmed by stashing that change and re-running
  the boss, but everything else in between is documentation or the per-tick
  decision below, which under `strict` is a provable no-op.
* **The guard is a setting**, since the comparison is worth being able to make by
  eye: `KF2_SMOOTH_OBJECTS_GUARD=strict|sticky|continuous`. It had a combo under
  Video ▸ Enhancements as well, and that came out when the four smoothing
  checkboxes were merged into one — a control for judging a picture is not a
  preference a player holds, and the console is where a comparison belongs. See
  "One switch for all of the smoothing" below.
  `strict` is 1024 on every table. `sticky` is the raise described above,
  creatures only.
  `continuous` additionally raises the cap on a slot that merely *moved* last tick
  rather than one that was *carried* — sticky's hysteresis is one-way, so a slot
  can only become sticky by first passing the bare threshold — which makes it the
  widest of the three and the worst for slot reuse. Switching mid-session drops
  the per-slot hysteresis so the new rule does not inherit the old one's
  decisions.
* **The carry decision is made once per tick, not once per frame.** The deltas are
  tick-constant so the answer does not change within a tick — but the hysteresis
  reads the same per-slot state the decision writes, so re-deriving it on the
  second frame of a tick would judge that frame against what the first frame had
  just stored. Under `continuous` that is a real divergence: frame 1 of a fast
  step refuses and sets `MovedLast`, and frame 2 would then find the raised cap
  and carry, moving the object in the middle of a tick.

Measured at 60 fps against the 20 Hz world, standing in an area: `121/121 frames
carried, 3.0 objects each, mean phase 0.40 tick, offset 14 u, biggest tick step 37
u`, with the probe's own leak check — re-read every touched slot after the renderer
and compare it with what the pre-hook wrote — reporting nothing. The **death clock
is untouched**, which is the check that matters: 65 ticks in **3219 ms** against
the 3250 ms a 20 Hz world owes. Cost, at a 1000 fps cap so there is something to
see: 550–615 fps either way, the run-to-run spread wider than the difference.

**Off by default**, the house rule for a mechanism that has been measured and whose
picture has not been looked at.

#### The player's arm is the same bug after all

**This section used to say the opposite, and the correction is the finding.** It
read: *"It is 2D, drawn by the HUD builder `func_80031D5C` out of the fourteen-entry
table at `0x80067774` … what steps is its sprite index … Interpolating between two
authored sprites is not a thing."* Every clause of that is wrong except "welded to
the screen".

The arm is drawn by **`func_80032400`**, a fourth drawing callee of stage 13 that
the census labelled "early 2D" and nobody opened. It is a **3D MO-animated mesh**
posed by the same clip clock as every creature: `func_80034DA8(0x8019949C, 0x20,
u8[0x801994AE], (s16)u16[0x801994A4], …)`, which forwards the clip byte and the
clip time to `func_8003486C` exactly as `func_80032588` does. The full layout is in
"What in the renderer draws what" in [GAME_INTERNALS.md](GAME_INTERNALS.md).

**Why the original measurement pointed at the wrong routine** is worth keeping,
because the method was sound and the reading was not. `func_80032400` returns
before drawing anything while the swing clock reads `-1`, so a census taken
standing in an area sees it draw nothing, and a thirteen-tick swing inside a
two-second averaging window barely moves it. The row that *did* move on pressing
attack — the HUD builder's — moved **downward**, 56.9 → 54.0 → 52.7 packets, which
is the wrong direction for an arm appearing: attacking collapses the HP/MP gauges,
and those are entries 9 and 10 of the HUD table. A difference was taken, the only
row that moved was believed, and the direction was not checked.

So `AnimSmoothing` grew a **second front-end** rather than a second patch — see
"The arm rides the same clock" below.

A creature's **pose** is an MO mesh morph, not a second copy of object
motion. `ObjectSmoothing` already carries origin and Euler on all four
tables the renderer walks. The clip byte and the clip time arrive at
`func_80032588` as its **eighth and ninth stack words** (`caller SP+0x1C` and
`+0x20`) — all five of `func_800331B4`'s call sites fill them, from four
tables with four different strides, which is why the argument list and not a
table offset is the right description. Below `0x80`, `func_80034DA8` applies
packed vertex deltas (the MO format) into `0x80190AD8`; `func_8003486C` turns
the integer time into a segment and a 12.12 weight.

Stage 13 rebuilds that morph every frame — `L80034FCC` re-runs the decoder
even when the segment has not changed — so the only thing stuck on the tick is
the time. `patches/AnimSmoothing.cs` (`KF2_SMOOTH_ANIM=1`) interpolates last
tick's time with this one, hands `floor(t)` to `3486C` so the segment pick
stays right, and adds the fraction onto the weight `34A74` already consumes —
**subtracting it on a segment whose flag word is set**, because `3486C`
publishes `0x1000 - raw` there and the weight runs backwards. It writes no
game state at all: one register on one call, and the caller's own stack temp.

The guard on the clip-time step is on its **size and not its sign**, and that
cost one bug each way: a magnitude of 32 against a real 511 made the first
version carry nothing at all, and treating a negative step as a discontinuity
left every clip played in reverse stepping at the tick rate — the drawbridge
lever went up in 50 ms jumps while the same lever came down smoothly. A clip
swap is caught by the clip byte; only the size separates playback from a
re-seek, in either direction.

**The end of a cycle is neither, and it was a third bug.** A looping clip runs
its time up and then resets it *keeping the same clip byte*, so the wrap arrives
as one ordinary tick whose step is a whole cycle backwards — and interpolating
that plays the animation in reverse, at cycle-per-tick speed, across every frame
of that tick. That is the rewind reported as "the animation snaps back to its
first position at the end of the cycle". The size guard cannot be what catches
it: a cycle shorter than 4096 slips under it, and one longer only turns the
rewind into a hard cut.

What separates a wrap from a re-seek is **where the time landed**, not how far it
moved. A loop turning over lands within one tick's advance of the cycle's first
frame — the overshoot past the end is what the new time is made of — while a
ping-pong clip easing back through its own last frames lands where it already
was, and a re-seek lands anywhere. So the test is `sign(step) != sign(lastStep)`
**and** `cur <= |lastStep|` (mirrored for a clip played in reverse, which turns
over at the tail).

Having recognised it, the tick is then run *forwards* through the turnover
instead of being skipped: the clip advances about `lastStep` a tick and the part
of that already spent in the new cycle is the new time itself, so the cycle
turned over `1 - cur/lastStep` of the way through the tick. Before that fraction
the old cycle is still finishing, after it the new one is running. **No clip
length is needed**, which is as well — the only place the total duration exists
is the segment table `func_8003486C` walks — and overshooting the real end by up
to a frame is harmless, because past its last segment that clock answers with
that segment at a full `0x1000` weight, which is the pose the clip ends on
anyway. A wrap's step is a cycle length rather than a rate and is deliberately
not recorded as `lastStep`, or the *next* wrap would be unrecognisable.

**The turnover is synthesised, not measured, and that has a cost.** It is made
out of `lastStep`, so a `lastStep` that is not a real playback rate invents one
wrongly — and a clip whose time is merely *jittering* near its own head reads as
wrapping every other tick, with the invented pose sweeping a fraction of the clip
and then cutting to the head. That is worse than the hard cut it replaces, so a
wrap is only believed off two consecutive same-direction ticks (`WrapRun`); with
less than that behind it the tick is held at the game's own time, which is the
hard cut.

**A clip being fought over is not animating.** The remaining case is an attack
whose animation the AI restarts every tick because the conditions to finish it
are never met — a piranha, or the final boss with the player under its head. Its
time steps one way and back the next, never landing at a cycle boundary, so it is
neither playback nor a wrap; interpolating it sweeps the pose *continuously*
between the two instead of alternating between them, which reads as a violent
shake where the console showed a 20 Hz flicker. Three reversals net of steady
playback (`ThrashFlips`) and the slot is held at the game's own time until it
resolves. That is not a repair of the game's own indecision — it is a refusal to
draw it more often than the game makes it. A ping-pong clip flips once a
half-cycle and decays back long before it gets there.

`KF2_SMOOTH_ANIM_PROBE=1` reports `N cycle wrap(s) (longest time seen T), M
carried through, R re-seek(s), S stuck` in these two modes, which is also how to
tell whether a clip is looping — or thrashing — through this path at all. In
`Mode.Timeline` the same line reports the new predicate instead: `N playback (W
on the wrap, T turned, B in reverse), H held (no match), S settling, L with no
clip length`.

#### The clip is a timeline, and none of that knew how long it was

Everything above — the landing-site wrap test, the synthesised turnover, the
`WrapRun` settling count, the `ThrashFlips` hold, the 4096 magnitude cutoff, and
the bounded mode below with its whole-tick overrun refusal — is a repair of one
missing fact. The patch was lerping the integer clip time as if it were a
Euclidean scalar on an unbounded line. It is not: it is a point on a **circle**,
and the circumference is the clip's own length.

**That length is not missing.** It is the sum of the per-segment durations in the
very table `func_8003486C` walks, and the whole record is reachable from the
`bank` and `clip` the clock is called with — clip table at `bank + u32[bank +
0x10]`, record at `bank + u32[clipTable + clip*4]`, `u16` segment count, then
`bank`-relative `u32` pointers to records whose `u16` at `+0x2` is the duration.
See "The model pipeline has no skeleton" in `docs/GAME_INTERNALS.md` for the
layout. Measured live, every clip reached in areas 0, 2 and 7 is **4096** long,
which the legacy probe confirms from the other side: the highest clip time it
ever saw on one is 4095, and its "cycle wrap" steps are 3936, 3942 and 4032
against playback rates of 160, 154 and 64 — `rate - 4096` exactly.

`Mode.Timeline` is that, and it is one predicate where there were five. Playback
is constant velocity along the circle, including through 0. So each tick:

1. **the clip byte changed** — a hard cut, show the game's pose, start again;
2. **else unwrap the step against the settled rate.** The observed `cur` is
   consistent with any step `cur + kD - prev`, and `k` is not searched for, it is
   `round((rate - step) / D)` — the wrap count that puts the candidate nearest
   the rate. Two more candidates cover a clip that **reflects** at an endpoint
   rather than wrapping: a turn at 0 lands at `-raw`, a turn at the end at
   `2D - raw`. The nearest candidate wins if it is within half the rate of it.
   That single test *is* ordinary playback, the cycle wrap, the reverse clip and
   the ping-pong turn;
3. **nothing matched** — a re-seek, or an attack the AI is restarting every tick
   — so hold at the game's own time, and re-seed the rate from what was actually
   seen so the next tick has something to confirm against.

A carried tick interpolates along the path it just recognised,
`raw = prev + delta * phase`, folds it back onto the clip (modulo for a wrap, a
triangle fold for a turn), hands `floor(t)` to `func_8003486C` and spends the
leftover fraction on the 12.12 weight. **The fraction is under one clip unit, so
it cannot leave the segment `floor(t)` landed in** — the overrun refusal the
bounded mode needs has nothing left to refuse, and the clamp on the weight is
only rounding at the very top of a segment.

Two things it gives up. A settled rate takes one tick to establish, so a slot
carries from its **third** sample of a clip rather than its second — except at a
genuine clip change, where the first moving step *is* the definition of the rate
and there is nothing to have re-seeked away from, so that one is taken on trust.
A step of zero is deliberately neither: it leaves both the rate and that trust
alone, or a clip posed for a tick before it starts would lose its opening step to
a rate of 0. And a clip whose length cannot be read never carries at all, which
is the honest failure rather than a guess.

##### The first version shook, and the probe had already said so

Shipped as the default, `Mode.Timeline` made the **teleport crystals shake
rapidly up and down** — reported from play, at 144 fps. The probe had recorded
the cause a run earlier and it was read as success:

```
76 playback (6 on the wrap, 0 turned, 0 in reverse)
73 playback (0 on the wrap, 4 turned, 0 in reverse)
```

Those two columns contradict each other. A clip that genuinely reflects at an
endpoint runs **backwards** for the rest of its half-cycle, so every real turn
must be followed by ticks counted `in reverse`. There were none, in any window,
while turns fired 1-6 times a second. **Every turn was spurious**, and a spurious
turn is a shake by construction: the pose runs to the end of the clip and back
inside one tick, once per tick, at the frame rate. A crystal whose clip is a
vertical bob renders that as exactly what was reported.

Three defects, and they are worth separating because only the first is about
turns:

1. **The rate was not reflected after a turn.** The unwrapped path length was
   stored as the new rate, keeping the *pre-turn* sign, so the tick after a turn
   always mispredicted, re-seeded and turned again. Self-perpetuating, and the
   direct reason no turn was ever followed by reverse playback.
2. **A turn was accepted merely for scoring closer to the rate than straight
   playback.** The reflection is a free extra parameter, so it wins on noise: a
   clip that simply *slowed down* near the end of its cycle was explained as a
   reflection. A turn is now considered only once constant velocity has failed
   its own tolerance **and** the clip would genuinely have overshot that end.
3. **The opening step of a clip was trusted with no bound.** A seek into the
   middle of a clip is indistinguishable from the start of a fast one except by
   size, and `Nearest(step, D, 0)` admits up to `D/2`. Measured playback is
   64-290 units against `D = 4096`, so `FirstStepFrac` refuses an opening step
   past a quarter of the clip — an order of magnitude clear of anything real.

**The structural lesson is bigger than the three bugs.** `Mode.Timeline` is the
only mode that can ask for a pose *outside* the interval `[prev, cur]`; that is
deliberate, and it is how a loop plays forward through its wrap instead of
rewinding. But it means a misclassification does not produce a slightly wrong
pose, it produces a pose from somewhere else in the clip. `Mode.Time`, for all
its ad-hoc classifiers, cannot do that: whatever it decides a tick was, it draws
something between two poses the game actually produced. **The two designs differ
in where the risk sits, not only in how principled they are** — a principled
predicate with an unbounded interpolator against guessy classifiers on top of a
bounded one — and the premise that the classifiers were the whole problem was
half the picture.

So the fallback is bounded too, which is the fourth fix and the one that makes
the rest safe. A carry is accepted within `RateTolRel` *of the rate*, so the
tolerance is as wide as whatever was last recorded — and the hold path was
recording whatever it saw, including a seek of up to `D/2`. That handed the next
tick a tolerance of `D/8` and let it accept a pose from anywhere. `Seed` now
refuses to call an implausible step a rate at all and leaves the slot unsettled,
so the slot draws at the tick rate until the clip does something a clip could do.
**What makes the mode safe is not that the predicate is always right; it is that
being wrong costs a held tick rather than a pose out of nowhere.**

##### A fast clip could never settle a rate, and so never moved

Play reported a **flying gecko's backflip looking like it ran at a low frame
rate** while everything around it was smooth. That is what `Mode.Timeline` looks
like when it holds — and it was holding, permanently, on that one animation.

`FirstStepFrac` was written as a bound on what the *opening* step of a clip may
be carried on trust. It was also being applied to whether the step could be
**recorded as a rate at all**, and those are not the same question. A clip whose
genuine playback rate is larger than a quarter of its length then cannot settle:

* the slot has no rate, so it takes the unsettled branch;
* the step is past the bound, so it is neither carried nor recorded;
* nothing changed, so the next tick reasons identically and holds identically.

It never recovers, for the whole animation. (A slot *past* its first step
recovered in two ticks, which is why this only ever showed on one clip: the
strand needs the very first step of a fresh clip to be a big one, and a backflip
starting from a new clip byte is exactly that.) The probe could not show it
either — a stranded slot is silent, and "no carries" reads identically to "nothing
animating".

Both halves are fixed. **Size buys a tick of latency; repetition buys
correctness.** A step too big to trust is now *made to wait one tick* and
confirmed by a second observation, which is what "settled playback" means and is
evidence that magnitude cannot supply; `Seed` records what it saw whatever the
size. The danger a big rate actually posed was never its size but that the
acceptance window scales with it, so that is capped directly instead
(`RateTolCap`, a twentieth of the clip — about one tick's motion, binding only
above a rate of ~410, well past anything measured). And `_maxRefused` is now in
the probe line as `widest refused N`, which is the counter that makes a stranded
clip visible at all: **a refused step of 1344 showed up in the first run after
adding it**, on a 4096-unit clip, so steps past the old ceiling are ordinary.

##### What it measures now

At `KF2_FPS=144` with `KF2_SMOOTH_ANIM_PROBE=1`, sweeping areas 0, 2 and 7 —
553 playback ticks over the run, **7 recognised as cycle wraps, 0 as endpoint
turns**, against 15 holds (the `0 slots settling` beside them was a dead counter
at the time and said nothing; see the review findings below); 381-527 weights carried a second
at a mean fraction of 0.55-0.59; `0 with no clip length` outside the frames of an
area transition. The load-bearing number is the **arc**: the widest step carried
over the whole run is **290 units**, the top of the measured playback range, so
nothing walked round the back of the circle, while the widest step *refused* was
1344 — the two staying far apart is the shape to want. Before the turn fix the
same scenes counted 1-6 turns a second. The same scenes under `Mode.Weight` show **8-17
carries a second refused for leaving their segment**, which is the defect
`Timeline` removes rather than bounds. The probe counts a morph submit with a
clip time of 0 separately (`N with a running clip`) so a scene full of MO-posed
props cannot read as success. Death clock unmoved: 65 frames in 3.16 s at 20 fps
with the mode on and off.

**`Mode.Timeline` is the default, and it took two readings by eye to get there.**
It shipped as the default, play reported the crystals; the default moved to
`Mode.Time`, which was the only mode with a positive report at that moment, while
the cause was found; with the four fixes in, play reports it looking very good
and it is the default again. The argument for it was always the stronger one.
What it lacked was the eye, and nothing in this repo could supply that.

##### Is it *correct*, though

On the invariant this was written to — *every in-between pose is a sample of the
game's own clip function between two consecutive ticks of playback, or else the
pose the game asked for* — `Timeline` satisfies it by construction and
`Mode.Time` does not. When `Timeline`'s predicate cannot explain a tick it
**holds**, which is the second half of the invariant; when `Mode.Time`'s
classifiers are wrong they still interpolate, and its turnover is *synthesised*
out of the last playback step rather than read, so the pose it draws across a
wrap is one the game never produced. That is a real difference in kind and it is
the honest reason to prefer `Timeline`.

"Objectively correct" is more than that claim, and four things are still open:

* **Three tuned constants remain** — `RateTolRel`, `RateTolAbs`,
  `FirstStepFrac`. Fewer than the five classifiers they replaced, and each is
  expressed against something measured (the slot's own rate, the clip's own
  length) rather than picked, but they are not derived.
* **"Playback is constant velocity" is an assumption about the game**, not a
  reading of it. It is strongly supported — 852 of 866 classified ticks were
  accepted as playback across three areas — but a clip that *eases* violates it
  and is held.
* **Two of the four cases the predicate claims have never been exercised.** The
  probe has never once counted an endpoint turn or a tick of reverse playback, in
  any run, before or after the fix. The ping-pong branch is the part of the
  predicate with no evidence behind it at all, and it is the part that shipped a
  visible defect; if anything shakes again, look there first. The gecko's
  backflip may well be the reverse case — the report was that it is the frontflip
  played backwards — in which case `0 in reverse` was a *second* symptom of the
  strand rather than a separate gap, and it should start counting now. Worth
  reading the probe next to that gecko.
* **Every clip measured is 4096 long.** The modulus is confirmed two ways (the
  highest clip time seen is 4095, and wrap steps are exactly `rate - 4096`), but
  a *uniform* length means the per-clip lookup would look identical if it were
  reading a constant. Finding one clip with a different `D` is what tests it.

##### Five things review found, and what each one was

Four of them are edges the mechanism never reached in any measured run, which is
the honest description of all but the last: they are guards, not repairs of an
observed picture.

* **A tick is a frame identity, not a flag.** `BeforeRenderer` advanced `_tick`
  on `FramePacing.TickedThisFrame`, which is decided at the frame boundary and is
  stable for the whole frame — so a *second* stage-13 walk inside one frame would
  step the tick twice, re-sample every slot at the same instant (a step of 0, a
  `Still`, no carry) and destroy the tick's real prev/cur pair. The identity that
  separates the two is `FramePacing.Frames`, which `SpriteAnim` was already
  keyed on for exactly this; it is now `FramePacing.FirstWalkOfTick(ref
  _lastFrame)`, shared, with the boundary watchdog **inside the helper** because
  the identity fails closed — with the boundary lost `Frames` freezes and a
  sampler would carry forever off one stale pair, so past 500 ms the guard drops
  and the flag decides alone, as it did before. `ObjectSmoothing.Before` had the
  same exposure at *three* sites, not one: its `Sample`, and the two hysteresis
  blocks whose own comment already says the decision is taken once per tick
  "because the hysteresis reads Gliding and MovedLast, which this same block
  writes" — a second walk in one frame is that case, and under `Continuous` it
  can admit a placement the first walk refused. All three now read one
  `tickFrame` local.

  **Measured: 0 re-walks**, at 144 fps through the autostart load and five area
  warps (0 ⇄ 2 ⇄ 7), reported as `N redrawn walk(s) not re-ticked` in the probe
  line. That is consistent with the code rather than with the guard being
  pointless: `LoopPacing.AfterRenderer` redraws inside `while
  (!FramePacing.TickedThisFrame …)`, so its redraws are non-ticked frames by
  construction, and every ordinary stage-13 walk ends in a `DrawOTag` that
  credits its own boundary. The reachable case is a walk whose `DrawOTag` is
  *not* credited — the boundary needs a preceding `VSync` — and nothing in these
  scenes produced one. So: cheap, correct, and its case unobserved.

* **The acceptance window's cap could undercut its floor.** `Window` read
  `Min(Max(RateTolAbs, |rate| * RateTolRel), d * RateTolCap)`, so for any clip
  shorter than `RateTolAbs / RateTolCap` = 40 units the ceiling pulled the result
  back under the floor — and a short clip is precisely what the floor exists for,
  so such a slot would fail its window every tick, re-seed and never carry at
  all. The floor is applied last now. Latent while every clip measured is 4096
  long, which is the same caveat as the bullet above it.

* **`_tFloor` was clamped below but not above.** Both folds can return exactly
  `Length` — `Mirror` returns `d` for a path that turned on the endpoint, and
  `Circle` reaches it by rounding for a `raw` a hair below zero on a reverse
  carry — and `Length` is *one past* the last valid clip time (the highest ever
  observed is `Length - 1`). Handing that to `func_8003486C` walks its segment
  list off the end of the array and returns whatever is past it, which
  `AfterClock`'s `segment == 0` test does not catch. Clamped at both ends.

* **The one write was the one address not checked.** `_weightPtr` is
  `m.ReadU32(c.SP + 0x10)`, taken on the assumption that the clock's caller is
  `func_80034DA8`; `_clockSeen` only guarantees the *first* clock call in a
  submit wins, not that the first one is that caller's, since `func_8003507C`
  reaches the same clock from elsewhere in the subtree. Guarded by `Ram()` now,
  as is the segment record `c.V0` the carry is scaled by — the same check
  `Duration` already puts in front of every load. The class doc claimed *"nothing
  here writes game state"*, which was true of the write's **lifetime** and not of
  the write, and is the sentence a future reader would have trusted; it now says
  what is actually guaranteed.

* **`{_unsettled} settling` was a dead column.** Every `Verdict.Hold` exit
  `Seed`s a rate on the way out, so by the time `Census` ran its
  `!s.HasRate || s.FirstStep` test every held slot looked settled and the counter
  could only ever print 0 — which matters because that 0 was quoted as evidence
  the mode was behaving. The distinction is only knowable at the exit that took
  it, so it is recorded there (`Slot.Settling`: true on the no-confirmed-rate
  hold, false on the rate-did-not-explain-it hold). **It reports now**: windows
  reading `1 settling` beside `0 held` and `5 held` in the warp run above, where
  before the fix the same runs read 0 everywhere.

  So the earlier reading of "0 slots settling" carried no information and is
  struck from the numbers below; the `held` figures beside it were always real.

What no counter here can say is the **picture**. Three cases need looking at: a
**looping** clip through its turnover (the wrap column proves the tick was
recognised, not that the pose is continuous across it), a clip played in
**reverse** — the drawbridge lever, which is what caught the old sign bug and
which the probe has still not reported a single instance of (`0 in reverse` in
every window) — and an **attack the AI restarts**, which is the case that lands
in the hold and should therefore look exactly like smoothing being off. The
crystals are now the fourth: they are the scene that caught the shake.

#### The bounded mode, and why it was the default

Play reported poses **spazzing out with interpolation on and never at 20 fps**,
and that reading is the one that matters: at 20 fps the phase is 0 on every
frame, so the carry is zero and the patch does nothing at all. The game's own
data is fine; the in-between pose is what is wrong.

Everything static analysis can settle says the mechanism is sound. The blender's
keyframe rebuild is **absolute rather than incremental** — `func_80034DA8` caches
on (clip, animation index, segment index) at `S1+0x2/+0x4/+0x6`, and a miss
rebuilds from a fixed per-segment keyframe list (`u16[seg+8]` through
`func_80034934`, then the list at `seg+0xA` accumulated by `func_800349F8`) — so
asking it for a segment out of order cannot desynchronise a decoder. The weight
slot really is the caller's: `func_8003486C` has no prologue, so its `SP+0x10` is
the blender's frame, where `SP+0x1C` was stored for it. The reversed-segment sign
is right. Also worth recording, since the doc comment had it loose: the submitter
passes the blender **three** distinct values — `a1` is the *bank* (from its own
`a1` register), `a2` the *animation index* (caller `SP+0x1C`, the byte tested
`< 0x80`), `a3` the *time* (caller `SP+0x20`) — and it is the animation index,
not the bank, that this patch keys slots on.

What **cannot** be settled from here is what the pipeline does when the segment
index moves at the *frame* rate rather than at the tick rate, and driving the
integer clip time is the only thing in this port that makes it do so.

So the default stopped doing it. `Mode.Weight` leaves the integer time exactly as
the game wrote it and spends the phase on the **12.12 blend weight alone**,
clamped to its segment: the frame stands `(1 - phase)` of a tick behind the pose
the game asked for, so `add = -(1 - phase) * step * 4096 / duration`. The pose is
then always between the segment's start and the pose the game asked for — it
cannot reach anywhere the game would not have drawn this tick, whatever the clip
time does — and the segment index, the cache and every decode are bit-for-bit
what they are with the patch absent. **That bound holds under every explanation
of the spazzing that could not be eliminated**, which is why it is the default
rather than one more guess at the cause.

It gives up motion *across* a segment boundary. **That has to be a refusal and
not a clamp**, and the first version of it clamped. A tick advances roughly one
segment (mean step 511 against `u16` durations), so on a clip whose segments are
short — a boss mid-attack — `(1 - phase) * step` is larger than the whole segment
for the early frames of every tick, and `Math.Clamp` turns those into the
segment's own *start* pose. The slot then holds still for part of the tick,
races through the remainder and jumps at the tick boundary: play reported the
final boss as **noticeably less smooth and more jittery with smoothing on than
with it off**, which is that, and it is a worse artefact than the 20 Hz stepping
the patch exists to remove. So a carry whose clamped value differs from the value
it asked for is dropped instead of written, and dropped for the **whole tick** —
the first frame of a tick carries the largest offset and so is the frame that
discovers the overrun, and a slot that carried on the later frames alone would
step *backwards* at the frame that stopped, which is the same jump one frame
later. A refused slot draws the pose the game asked for, identically to the patch
being off. `KF2_SMOOTH_ANIM_PROBE=1` reports `N refused (left the segment)`
beside the carry count; a clip whose segments are long enough never shows it.

Carrying properly across the boundary means picking the *earlier* segment, which
is precisely what `Mode.Weight` exists in order not to do, so within that mode it
stays a refusal. `Mode.Timeline` picks the earlier segment on purpose and bounds
the result a different way — the fraction it spends on the weight is under one
clip unit, so the segment `floor(t)` landed in is the segment that gets drawn —
which is why the refusal has nothing left to catch there.

**All three are selectable, because only the picture can separate them** — which
is not a formality here: the mode with the best argument behind it is the one
play caught shaking, and the switch is how that was found. The switch is
`KF2_SMOOTH_ANIM=timeline|weight|time` sets it from the console, which since the
merge below is the only place it is set from; switching clears
the per-slot state, so a creature on screen steps for one tick and then draws
under the new mode, which is what makes an A/B while something is animating
possible at all. **`Time` is the default**, because it is the only mode with a
positive report by eye. When the picture has been judged, the losers go.

Vertex-fetch lerp at `RotTransPers` was tried first and did not change the
picture — it interpolated a rigid majority, and the probe's "XYZ moved" line
was vertex 0 only. Measured not to touch the world clock, which the discarded
version did: 65 death frames in 3.25-3.28 s with it on at 20, 60 and 144 fps,
against 3.25-3.27 s with it off.

**The arm is not a sprite index and does not stay.** See "The player's arm is the
same bug after all" above, and "The arm rides the same clock" below.

#### The arm rides the same clock

`func_80034DA8` hands `func_8003486C` the clip byte and the clip time whoever
called it, so the arm needs no machinery of its own — only a way into the window
`BeforeClock`/`AfterClock` already work inside. That is two hook pairs and one
refactor:

* `BeforeSubmit`'s body below its argument reads became **`Observe(id, clip, time,
  rootCoupled)`**. Everything above that line is "where do the clip byte, the clip
  time and the slot identity live in *this* call"; everything below is the same for
  a creature and for the arm. `rootCoupled` is false for the arm — the hold that
  keeps a pose with a root `ObjectSmoothing` refused only means anything for
  something whose root is in one of those tables, and the arm is placed from the
  equipped weapon's record instead.
* A pair on **`func_80032400`** sets `_inArm`. That is the scope fence:
  `func_80034DA8` has four callers — the arm, the HUD builder and both object walks
  — and only the arm is carried here.
* A pair on **`func_80034DA8`** does for the arm what `BeforeSubmit` does for a
  creature: refuse when `_depth != 0` (the creature path already has that call in
  hand), open the window, and `Observe(a0, a2 & 0xFF, (s16)a3, rootCoupled: false)`.
  The slot key is `a0` = `0x8019949C`, the MO decoder cache slot, which is disjoint
  from every position pointer the creature front-end keys on.

**The idle frames are the one thing the arm needs that a creature does not.**
`func_80032400` draws nothing while the clock is `-1`, so the carry never sees the
gap between two swings; and the clip byte is the *kind* of attack rather than the
attack, so a second swing of the same kind would arrive as one enormous backwards
step off the end of the first and be classified as a re-seek for the whole of it.
The pre on `func_80032400` reads the clock itself, and resets the slot once per
idle gap. That needs no rule about what a swing looks like.

Nothing is written to game memory — one register on one call, plus the blender's
own stack temp, the same as the creature path — so there is no restore, no leak
check and no ordering constraint against `LoopPacing`. A redraw inside a modal loop
replays stage 13, so the arm is carried there too, for free.

Measured at `KF2_FPS=144` against the 20 Hz world, twenty-odd swings across areas 1
and 2, in both `Mode.Timeline` and `Mode.Time`: **clip 0, step 300 a tick, 4096-unit
clip, 13 ticks a swing, 0 held, 0 rigid**, and 86 of a swing's 94 rendered frames
carried — the uncarried ones being the opening tick, which has nothing to
interpolate from. The world clock is untouched: 60 death ticks in 3015 ms (19.9/s),
and the swing takes the same thirteen ticks and reaches the same clip time with the
carry on and off, which is the more direct check of the two.

The probe (`KF2_SMOOTH_ANIM_PROBE=1`) reports the arm on **its own line**, because
one slot against a scene's worth of creatures would otherwise vanish into the
rounding, and because the two questions it has to answer are its own: is the swing
a morph clip at all (`rigid 0`), and is its time moving (`step 300`). An idle second
reads `arm no swing`, which is deliberately not the same answer as `rigid`.

It rides `KF2_SMOOTH_ANIM` and the one Video smoothing tick
— same mechanism, same predicate, same switch — and so is **on by default** with
the rest.

### The menu's cursor repeat is outside the gate by construction

The stage gate is a pre-hook on six main-loop entry points, so it can only decide
whether one of those six *runs*. The in-game menu is not one of them and never
passes through them: `func_80029CBC`, inside stage 3, `jal`s **`func_80018E80`**
on a just-pressed Circle, and that call **blocks for the whole menu session**,
running its own loop and presenting its own frames through `func_800226A8`
(`VSync` then `DrawOTag`). While it runs, the main loop is parked inside stage
3's `jal`; `FramePacing.AfterDrawOTag` still fires and the accumulator still
ticks, but `BeforeStage` is never consulted, so **nothing inside the menu is on
the tick clock**. This is the case the "Three things held the port at 30" section
already flags: skipping stage 3 decides whether such a loop is entered, it cannot
cut one in half. (Stage 3 reaches the renderer by more paths than that section
originally claimed — `scripts/check_gate.py` enumerates them — but they are all
extra renders inside the stage rather than the frame's own.)

What that cost is the cursor. The two steppers — `func_8001EA14` (a fixed option
list) and `func_8001EB70` (a scrolling one: inventory, equipment, magic) — open
the same way, and there is **no edge detection** in either:

    func_80022E90();   // the auto-repeat delay
    func_80022E58();   // PadRead(1), and latch 0x8006E5C4 if anything is down
    ... test Up (0x8006E590) / Down (0x8006E594) against that word, and step

Holding Up steps the cursor on every iteration of the menu loop. The only
throttle is **`func_80022E90`**:

    if (*0x8006E5C4 != 1) return;          // nothing was down last read
    *0x8006E5C4 = 0;
    for (s0 = 0; ; ) {
        if (PadRead(1) == 0) return;       // released
        if (s0 < 6) { s0++; VSync(0); continue; }
        *0x8006E5CC = 0; return;           // the repeat fires
    }

On hardware `VSync(0)` waits for the next vblank, so that spin costs **six
vblanks — 100 ms** whatever frame rate the game itself was achieving. Since
[`0021`](RUNTIME.md) the emulated vblank is a wall-clock grid and `VSync(0)`
presents and returns, so the only thing pacing a VSync *call* is `FrameClock` —
which `FramePacing.ApplyHostCeiling` deliberately sets permissive at
`max(60, TargetFps * 2)`, because it paces per call and a frame can carry more
than one. The delay inherited that ceiling, so raising the render rate shortened
it — and above 60 the ceiling stopped holding the spin at all.

Measured by holding Down in the menu for two seconds, driven over `KF2_SHELL`,
with `KF2_MENUPACING_PROBE=1` reporting what each spin cost:

| `KF2_FPS` | spin, unpaced | steps/s | spin, paced | steps/s |
|---|---|---|---|---|
| 20 (default) | 66 ms | 7.5 | 100.8 ms | 6.0 |
| 60 | 41 ms | 15.0 | 100.7 ms | 8.5 |
| 144 | 1–17 ms | **36–37** | 100.2 ms | 9.5 |

The 144 row is the complaint: thirty-seven steps a second through a list. **Read
the steps/s column, not the spin.** The unpaced spin is not reproducible — the
same configuration measured 1.2 ms on one run and 16.6 ms on the next — while the
repeat rate it produced was 37.5 and 36.0 across those same two. And it is **not**
the ceiling's arithmetic either: six calls at a 288/s ceiling would be 21 ms. The
ceiling is a per-call throttle that the menu's own frame is already spending
against, so predicting the number from the rate is exactly the mistake `0025`
warns about. The honest statement is that the delay was on the frame clock, and
the frame clock does not hold it.

`0025`'s own comment names the trap: the ceiling is permissive *because* it paces
per call, and a caller that needs a rate should keep its own deadline at the frame
boundary. `FramePacing` does; this delay is expressed in calls, so it did not.

**`patches/MenuPacing.cs` makes those six calls cost a vblank each again, and only
those.** One pre/post pair around `func_80022E90` marks the window, and a pre on
GAME.EXE's `VSync` thunk (`0x8005FCC8`) holds to the next 1/60 s boundary while it
is open. The six frames are still presented, which is the point of pacing the
calls rather than sleeping the shortfall afterwards — that would be ~79 ms of
frozen picture every repeat at 144 fps. `LibEtc`'s own `_vcount` cannot be the
clock, since it only advances *from* a `VSync` call and waiting on it would
deadlock, so the patch keeps its own 60 Hz grid.

**The residual is one menu frame**, and it is deliberate: 6.0 steps a second at
20 fps against 9.5 at 144, because after the spin returns the menu still renders
one frame at the render rate — 50 ms against 7 ms on top of a constant 100. That
is a 1.6× spread replacing a 5× one. Closing it entirely would mean pacing every
`VSync` inside `func_80018E80` rather than inside the repeat, which pins the whole
menu to 60 fps; not done, and one line away if the residual ever reads as a rate
dependence rather than as feel.

Three things about the shape are load-bearing. Adding `func_80018E80` to the gate
would skip the **entire menu session** rather than one iteration of its loop,
because `HookManager` detours whole functions. Gating the steppers is worse: both
return the new cursor index in `V0`, so a bare `return false` hands the caller
garbage. And it costs nothing when idle — `func_80022E90` returns before its loop
whenever `0x8006E5C4` is not 1, so nothing is paced unless a direction is actually
held. One hook covers every list in the game: `func_8001EA14`, `func_8001EB70`,
`func_8001B0D0`, `func_8001BB7C`, `func_8001BE60` and `func_800206E0` all call it.

It is on by default with no settings page — a correctness fix like frame pacing
rather than a taste like dithering, and the console fixed the number at one value.
`KF2_MENUPACING=0` is the comparison; `KF2_MENUPACING_PROBE=1` prints what each
repeat cost and how fast the blink stepped.

### The blink is the same bug one layer up

The cursor's highlight is an eight-step ramp up and back down. `func_80022530` —
the menu's **frame head**: buffer swap, OT pointer, `ClearOTag` — steps
`0x8006E5CC` by one in the direction at `0x8006E5D0`, clamping to 7 at the top and
latching `0xFFFFFFFF` at the bottom, and `func_80021A84` reads the counter as
`(v + 0x1F4) << 6` into a sprite's `+0xE` to pick one of eight cursor frames.
`func_8001EA14` zeroes the direction on every accepted move and `func_80022E90`
zeroes the counter when a repeat fires.

Two things about it were not what they looked like. It is **not a continuous
pulse** — the down ramp latches the direction off, so it is one wink per accepted
move, sixteen steps long, and sitting still in a menu steps it zero times a second
(measured). And the frame head runs **twice per iteration** of `func_80018E80`,
because the menu's inner loop makes two passes and each one presents.

It steps once per menu frame, so its rate is the render rate. Measured steps a
second while holding Down:

| `KF2_FPS` | unpaced | capped |
|---|---|---|
| 20 (default) | 15–19 | 8–13 |
| 60 | 30–35 | ~21 |
| 144 | 73–77 | 9–20 |

The frame head cannot be skipped — it swaps the buffer — so the fix is a pre/post
pair saving the two words and putting them back on a frame the grid did not
advance on, the same shape `ObjectSmoothing` uses. **Nothing sleeps for the
blink**: the menu still renders at the render rate, only the counter is held, so
this caps the wink rather than pacing the menu. That is what keeps it from
becoming the 60 fps menu the repeat fix deliberately avoided.

**60 Hz and not `LogicHz`, deliberately**, for the reason the repeat gives: a menu
frame is one `VSync(0)`, which is one vblank, and the tick rate is a judgement
about what the *world* achieved under load. Binding it there would make
`KF2_TICKRATE` — a setting about game speed — retune the interface.

**It does bite a little at the 20 fps default**, and that is worth recording
because the first guess was that it would not. The menu renders its frames in
pairs, and the second of a pair lands inside the same vblank slot as the first
whatever the render rate, so one of the two is always held. The default's wink
gets somewhat longer rather than staying exactly as it was.

**The cap is a choice, not a reading.** If the console's menu held 60 fps it
stepped the blink twice a vblank — the frame head runs twice an iteration — and
the faithful cap would be 8.3 ms, a 0.13 s wink instead of 0.27 s. That rests on
an assumption about a frame rate this port cannot observe, and the complaint being
fixed is "too fast", so the slower of the two is the default. `MenuPacing.BlinkMs`
is the one constant to change. **By eye is the only way to settle it, and it has
not been looked at.**

Every other modal loop was rate-dependent for the same structural reason, and
that list — the menu box open/close animation (`func_800356F4`), the transition
fade (`func_80037B5C`), the cutscene and message-box loops (`func_80047000`,
`func_80048208`, `func_8004831C`) and the spell-cast and item-use animations
(`func_800474D0`) — is what stopped being enumerated by hand. See "Loops that
render their own frames" below.

### The tints strobed between ticks

**Mechanism measured; the picture has not been judged by eye.** Reported from play
as flashing on death. The death fade and the damage flash are one request block at
`0x80192D45` (mode, R, G, B; "The screen-space effects are 320 wide too" in
[WIDESCREEN.md](WIDESCREEN.md)), read by `func_8003214C` in stage 13 every frame.
**Stage 1, `func_8002C944`, resets it at the head of every main-loop iteration** —
mode `0xFF`, and the second wash at `0x80192D49`..`0x80192D4F` zeroed — and the
stages that ask for a tint are 2 and 3, both gated to the tick. So above the tick
rate the tint was drawn on the tick's frame and cleared on each frame after it.
Stage 1 was on the list of ungated stages nobody had checked (`check_gate.py
--stages`, `docs/TODO.md`).

The modal fade `func_80037B5C` does not strobe, because it sets the tint and calls
stage 13 itself with no stage 1 in between; only a tint asked for from the main
loop does. With auto reload on, the death fade's `32..64` arm is held at 31, so
what a player saw strobe was mostly the hit flash and anything else the gated
stages tint.

Measured at 144 fps, `KF2_AUTORELOAD=0`, a simulated death: 32 of 231 frames in
the fade carried the tint (one frame per tick), against 231 of 231 with the hold.
144.0 fps drawn at 20.0 ticks/s either way.

**Stage 1 is not gated whole**, because it also copies 80 records from
`0x800679A0` into `0x801930F0` and clears the load flag at `0x801930EC`, which
stage 10 (ungated) reads; holding that flag across the frames between ticks would
run stage 10's per-record work several times a tick. So `patches/TintHold.cs`
replaces stage 1 with a C# copy of it that does the copy and the clear every
frame, as before, and the tint reset only when the gated stages will run — the
reset is moved onto the tick with them rather than undone afterwards.
`FramePacing.StagesWillRun` answers that question with no side effect: the same
decision `BeforeStage` returns on the live boundary. With the boundary lost the
decision is made only inside `FallbackTick`, so it answers true and the routine
behaves as the game's own, strobe included, in a state that already announces
itself. A pause holds the tint too. PGXP's CPU tracking falls back to the
recompiled routine.

`KF2_TINTHOLD=verify` runs both on every call, with the reset on in ours, and
compares all of RAM and the registers the routine leaves; the recompiled result
stands. Measured over boot, the slot load, a death and the respawn: 16 windows of
288 calls, 0 RAM and 0 register mismatches. `KF2_TINTHOLD=0` is the recompiled
routine, strobe and all.

### Loops that render their own frames

**The generalisation of the two fixes above, and the reason the list stopped
growing by bug report.** Reported from play at a high rate: a picked-up item spins
too fast, an NPC's interact animation plays too fast. Both are the cursor's bug
with a different counter in a different loop, and the answer had to be structural
— finding the counters by playing the game is exactly what the two fixes above
cost.

**Why a modal loop escapes the gate.** `FramePacing` holds the world to `LogicHz`
by *skipping* five main-loop stages on a non-tick frame. A modal loop is entered
*from* one of those stages, so the gate decides only whether it is entered and
never cuts one in half. Inside, no gated stage is being called: the loop iterates
once per **rendered** frame, and everything it steps — a rotation, a fade level, a
sprite index, a message timer — runs at the render rate.

**The fix is one sentence.** On the console a modal loop's iteration *was* a
rendered frame and a rendered frame *was* a tick. The port broke that identity
everywhere the stage gate cannot reach, so put it back: **a modal loop's body runs
once per world tick, not once per rendered frame.** Nothing is enumerated, nothing
is snapshotted and no counter has to be found — the loop iterates as often as the
world ticks, so every number inside it is right by construction. It is the
smoothing fix run backwards: smoothing takes the phase between two ticks and *adds*
frames the game did not compute, this takes the same phase and *withholds*
iterations it should not have computed.

### Holding the loop is half of it, and the first version shipped only that half

The half that shipped paced the loop's *frames* to the tick rate, which is the
same thing as holding its body — one iteration, one frame — and it is what came
back from play: *"it runs at the correct speed, BUT the animation is at its
original framerate, and so is the camera."*

Of course it was, and the reason is already written down two sections up. The
frame the loop drew **was** the tick, so `FramePacing.LogicPhase` was 0 on every
one of them and `FrameSmoothing`, `ObjectSmoothing` and `AnimSmoothing` had
nothing to carry — exactly the state everything was in while the frame boundary
was broken and "the smoothing never ran at all". Pacing gives a modal loop the
console's *speed* and the console's *frame rate*, and the port's whole argument is
that those two should stop being the same number.

So the loop is not paced. It is **gated**, and the gap between its iterations is
filled with **redraws**: stage 13 called again at the phase the frame now stands
at. That is not a new mechanism. `func_80037B5C` already renders extra frames
inside a stage by calling stage 13, and that is precisely why stages 2 and 3 are
recorded gate exceptions; this does the same thing deliberately.

What it buys is that the world advances at `LogicHz` while the picture is drawn at
the render rate, and `ObjectSmoothing` and `AnimSmoothing` — which bracket stage 13
— carry the objects, the creatures and the poses between ticks the way they already
do everywhere else. Neither re-reads memory on a non-tick frame (each keeps `prev`
and `cur` in managed fields and only re-samples when `TickedThisFrame`), so a
redraw is safe for them by construction; it only re-lerps at the new phase.

The loop ends on the logic clock rather than on a counter: each redraw passes the
frame boundary itself, so it is paced by `FramePacing.Floor` and advances the
accumulator exactly as an ordinary frame does. `MaxExtraRenders` is a backstop
against a configuration nobody has thought of, not the mechanism.

### A redraw has to pass stage 13 its two pointers, and the first one shipped did not

Stage 13 is **`func_800342D8(VECTOR *pos, SVECTOR *rot)`**. It opens with
`func_8002E22C`, which copies 16 bytes from `a0` and 8 from `a1` into
`0x80192E78`/`0x80192E88` and builds the frame's whole view matrix out of them —
**unless both are zero, in which case it reuses what is already stored**. Stage 8,
`func_80025A1C(pos, rot)`, is the routine that *fills* those two blocks: 12 bytes
written through `a0`, six through `a1`. The main loop `func_8001369C` keeps two
stack scratch blocks (`s1 = sp+0x28`, `s0 = sp+0x38`) and hands the same pair to
both stages; `func_80037B5C` does the same with `sp+0x10`/`sp+0x20`.

The first version of the redraw called `stage8(c, m)` and `stage13(c, m)` with
whatever the register file held. After stage 13 that is the tail of
`func_8003549C`, so `a0` is a pointer into the sound-slot table near `0x8018EAA4`:
stage 8 wrote the camera *into live game data*, and stage 13 then projected the
world through it. A garbage view matrix draws next to nothing, and this game's
`PutDrawEnv` has `isbg=0` — there is no background clear, the game overwrites the
buffer by drawing the whole scene — so a mostly-empty frame leaves **the previous
contents of that buffer** on screen. Double-buffered at seven redraws a tick, that
is what came back from play: constant black flicker, no camera interpolation, and
"the frame you died on" alternating with the live picture.

So a redraw **replays stage 13 with the arguments the modal loop itself passed**,
recorded by a pre-hook on the same function. That is literally "draw this frame
again at a later phase", and it is right for all three call shapes in the game: the
fade's own stack blocks (still live below `sp`, and unchanged because the world is
frozen), the `0, 0` of the item-use and `fdat05`/`fdat14` loops, and `fdat23`'s
scripted cutscene camera. The register file is snapshotted and restored around the
redraws, so the loop resumes with exactly what stage 13 left it.

**Stage 8 is deliberately not replayed.** It would overwrite a cutscene's scripted
camera with the player's, and it buys nothing: no gated stage runs inside a modal
loop, so the *player* camera cannot move and `FrameSmoothing` has nothing to carry
there. Measured — `KF2_LOOPPACING_PROBE=2` reads `the loop's own view moved 0.0 u
(peak 0) and 0.0 units per iteration` through a transition fade, a warp and the
menu, over every window but the one that spans an area load.

### A camera the loop builds itself is the one that does move

Play reported it in one line: *"the camera is visibly moving at a lower framerate
in the modals."* `func_8004831C`, the cutscene and message-box loop, has two:

* one ramps a heading `0 -> 0x1000` by `0x200` an iteration and hands stage 13 the
  `u16` it has just written (`a0 = 0, a1 = s2`) — a **full turn in 32 steps**;
* the other steps `rec+0x26` by `0x40` an iteration and passes
  `a1 = 0x80199504`, the player's own composed view, the address `FrameSmoothing`
  already knows.

Held to the tick that is 11.25° a step against a 165 fps picture. It is not a
regression — it is the *speed* fix arriving without the smoothing half, the same
shape as every other complaint this port has had.

So a redraw **carries it**, in the shape the other two smoothing patches already
use. `LoopPacing` keeps the block the loop passed at the previous iteration and at
this one and draws `lerp(prev, cur, phase)` — **interpolated**, at `t - 1 + frac`,
so it agrees with `FrameSmoothing` and `ObjectSmoothing` and can never reach a
heading the loop did not produce. Three `u16` angles at `a1` and three words of
position at `a0`, which is exactly what `func_8002E22C` consumes; applied in the pre
and taken back in the post, so the interpolated values live for one stage 13 call
and the loop's own state is never touched. A step past `CutUnits` (1024, a quarter
turn, eight times the largest pan measured) is a **cut** rather than a pan and is
left alone, the way both other patches guard a placement.

Re-primed whenever the pointer pair changes, an overlay loads, or the main loop
draws a frame — that last one is also what keeps the carry out of the main loop
entirely, since `_carriable` can then never be true there and `FrameSmoothing`
stays the only thing carrying the player's view.

`KF2_LOOPPACING=nocarry` is the A/B: redraw, but leave the loop's own pan stepping
at the tick.

Measured at `KF2_FPS=165` with `KF2_LOOPPACING_PROBE=1`, which now prints the
recorded pair: the death fade ran at **165.0 modal world frames a second against
19.9 loop iterations, 7.3 redraws each**, and every window reported
`view a0=0x801FFEE8 a1=0x801FFEF8` — a stack address, the loop's own scratch. The
menu came out at 60.0, and at `KF2_FPS=20` the redraw count is **0.0**, which is
the "does nothing at or below the tick rate" claim in one number. Menu, warp, death
and auto-reload raised no exception and no unmapped call.

**Two things it costs, both stated rather than discovered later.** Whatever stage
13 steps in its *own* body now steps once per rendered frame inside a modal loop —
the jitter accumulator at `0x8006E608` and `func_800331B4`'s ambient-sound
retrigger — which makes a modal loop no worse than an ordinary frame rather than
better; both are already open in [TODO.md](TODO.md). And a redraw cannot reach a
counter the modal loop steps in its own body: a picked-up item's spin, or a
cutscene camera the loop pans itself, steps once a tick, which is the console's own
rate for it, and smoothing *that* would need the model submit's arguments
interpolated rather than a table.

**The reprime is on the marker, not on the frame boundary.** One line keeps
`_carriable` false during ordinary play: `Sample` sets `_primed` on every stage 13
call, and a reprime clears it again, so `_carriable = _primed` reads false on
every main-loop frame. That reprime used to sit in `FrameMinMs`, which has exactly
one call site — inside `FramePacing.AfterDrawOTag` — so it rested entirely on the
frame boundary being reached, and a lost boundary is a failure this repo has hit
twice (patch `0027`, and the watchdog `FramePacing.BeforeStage` now carries for
precisely this). With the boundary gone `_primed` stays true across frames,
`_carriable` latches, and `Carry` starts interpolating stage 13's view **in the
main loop**, at a frozen phase — `_logicCredit` is advanced by
`AdvanceLogicClock`, on the same dead path — which is a constant-lag camera on top
of `FrameSmoothing`'s own. It now runs in `MainLoopStage` itself, which *is* the
signal that the main loop is running: stage 9 sits after every stage a modal loop
is entered from and before stage 13, so the main loop's own frame still finds
`_primed` clear. `Carry` additionally refuses outright when the marker is not
hooked, because the reprime then never runs at all and the class has already stood
itself down.

**Install order is load-bearing.** `HookManager` runs the posts on a function in
the order they were added, so `LoopPacing` is installed in `Program.cs` *after*
all three smoothing patches: the redraw has to be asked for once their posts
have put the tables back, not while their interpolated values are still in them.

**Classifying a frame costs one hook.** `FramePacing.AfterDrawOTag` already fires
on a modal frame, because a modal loop presents through `func_8002E0FC` (stage 13)
or `func_800226A8` (the menu) and both `VSync` and then draw the table — so the
whole defect is that `Floor` was pacing that frame to `TargetFps`. Two questions
decide what it should pace to instead:

* **Is this the main loop's frame?** A pre-hook on **stage 9, `func_800140AC`**,
  whose *only* caller is `func_8001369C`, the main loop. Stages 3, 4 and 6 are the
  only others with a single caller; stage 1 looks like the obvious marker and is
  the wrong one — it is also called from two modal loops and three area modules.
  Stage 9 sits after every stage a modal loop is entered from (2, 3, 7), so a
  loop's own first frame classifies correctly, and it is not in the gate set, so
  `KF2_FPS_GATE` cannot disturb what it measures.
* **Did this modal frame draw the world?** The game's own frame gate
  `func_80017880` is called by stage 13 and by nothing else, and
  `FramePacing.BeforeFrameGate` already hooks it — so that answer costs no hook at
  all.

**The absence of the marker is not evidence of a modal loop outside GAME.EXE.**
Both answers above are GAME.EXE addresses — the marker is `0x800140AC` and the
frame gate `0x80017880`, and neither routine exists in OPEN.EXE or END.EXE — but
`FramePacing.AfterDrawOTag`, the one caller of `LoopPacing.FrameMinMs`, is hooked
in **all three** overlays (`open 0x80016078`, `game 0x80060818`,
`end 0x80013D80`). So every title, save-select, intro and ending frame arrived
with both flags clear, fell through both guards, and was paced at `InterfaceHz`:
**measured, the title screen ran at exactly 60.0 frames a second with
`KF2_FPS=144`**, and `if (mainLoop) Reprime()` never fired there either. It also
contradicted `FramePacing`'s own stated intent for the same pair of overlays —
"only GAME.EXE's copy is hooked, so the title and the ending pace themselves as
they always did".

`LoopPacing` therefore tracks which **executable** is loaded, from
`OverlayLoadedEvent` (`"game"` on, `"open"`/`"end"` off) — the nine `fdat` area
modules are GAME.EXE still running and are left alone, the same distinction
`patches/Widescreen.cs` draws for the margin latch — and a frame outside GAME.EXE
classifies as the main loop's, which is the branch that returns `FramePacing`'s
own deadline unchanged. Measured with `KF2_LOOPPACING_PROBE=1`: before, OPEN.EXE
reported 15 (the intro movie, disc-paced) rising to a flat **60.0** modal
interface frames a second once the title was reached, and END.EXE 10.0; after,
neither overlay reports a modal frame at all. GAME.EXE's own save-select menu
still reads 49.7-60 modal interface, which is the class doing its job.

### The title menu is an interface frame

The fix above stood OPEN.EXE down entirely, so the title menu — which has no wait
of its own — drew at whatever `KF2_FPS` asked, and uncapped meant flat out. It
draws nothing but interface, so it is now held to `InterfaceHz` the same as
GAME.EXE's menus: `FrameMinMs` returns `max(1000/T, 1000/60)` while OPEN.EXE is
the loaded executable (`_openExe`, from `OverlayLoadedEvent`), before any of the
modal classification, which still does not apply there. The logos' STR stream and
the intro movie are disc-paced at 15 and 10 and are below the cap. END.EXE is
unchanged. `KF2_LOOPPACING=0` is the comparison.

Measured at `KF2_FPS=144`: the logos 15.0, the title **60.0 drawn, 60.0 presents
a second**, and after `fdat05` 144.0 drawn at 19.9-20.0 ticks a second. The
render rate below 60 is untouched, since the longer deadline wins.

**The interface is the other case, and it is paced rather than filled.** A modal
loop that draws no world — the menu — has nothing for the smoothing patches to
carry and never calls stage 13, so there is no gap worth filling. It is paced, at
**60 Hz and not the tick rate**, for the reason `MenuPacing.BlinkMs` already gives:
a menu frame is one vblank, and `KF2_TICKRATE` is a setting about game *speed* with
no business retuning a cursor. The menu also presents twice per iteration of
`func_80018E80`, so binding it to a 20 Hz tick would make it respond at 10 Hz.
Pacing is also the **fallback** for a world-drawing loop if stage 13 cannot be
resolved or has not yet been seen with its arguments, and it is what
`KF2_LOOPPACING=pace` selects on purpose, as the A/B against the redraws: the speed
stays right and the picture goes back to stepping, which is the failure worth
having.

Measured with `python3 scripts/rate_matrix.py modal-rate --fps 20 144`, which
opens the menu and then warps, against the same run with
`--env KF2_LOOPPACING=0`:

| `KF2_FPS` | main/s | modal world/s | world iter/s | modal ui/s |
|---|---|---|---|---|
| 20, off | 15.0 | 21.0 | 21.0 | 20.1 |
| 20, on | 15.4 | 21.0 | 21.0 | 20.0 |
| 144, off | 111.9 | **33.8** | **33.8** | **144.0** |
| 144, on | 142.9 | **144.0** | **19.9** | **60.1** |

The two world columns say different things and both are load-bearing.
`world iter/s` is the loop **body** — the thing that was running too fast, and
which has to equal the tick rate. `modal world/s` is the **picture**, which has to
equal the render rate. Off, they are the same number, which *is* the defect; on,
they separate into 20 and 144. The 20 fps rows are the point of the "does nothing
at the tick rate" claim: identical either way. `main/s` is a maximum over windows
that included modal time and so reads low whenever a loop ran for part of one —
read the modal columns, not that one. The unpaced world row is 33.8 rather than
144 because a fade frame is a full world render and never reaches the target: it
was 1.7× too fast, and this class of complaint scales with whatever the loop can
achieve rather than with the number asked for.

**The redraw cap is derived, not pinned, and it says something when it is hit.**
`AfterRenderer`'s loop really ends on the logic clock, which is wall-clock driven
and always ticks, so the count is a backstop — but it was a constant 64 justified
against the 20 Hz default ("over three ticks' worth at 1000 fps") while the
redraws one tick needs are `TargetFps / LogicHz` and **both ends are settings**.
`FramePacing.ClampLogic` allows `KF2_TICKRATE` down to 5, so that ratio runs to
`1000 / 5 = 200` and 64 becomes a *third* of one tick: the cap is reached before
the tick arrives, the loop body runs anyway, and it iterates faster than the world
— the defect this class exists to remove, silently back. `MaxRedraws()` is three
ticks' worth of `max(TargetFps, FramePacing.Measured) / LogicHz`, floored at the
old 64; `Measured` stands in when the rate is uncapped and there is no target to
divide. Hitting it now prints one line, once, the way `FramePacing`'s boundary
watchdog and `SpriteAnim`'s do — it used to be recorded only in `_extra`, and only
if the probe happened to be on.

This is hardening rather than a measured behaviour change: on this host the
reachable case does not reach the old cap either. `KF2_TICKRATE=5` with
`KF2_FPS=off` measured 60.0 modal world frames a second against a 5.9 Hz loop
body — twelve redraws a tick, not sixty-four — because a fade frame is a full
world render and uncapped throughput in one is around 60. The finding's premise
that a modern GPU "comfortably clears 320 fps" **in a fade** is what did not hold;
the arithmetic that makes 64 wrong still does.

**It does nothing at or below the tick rate**, which is where the defect does not
exist either: `LoopPacing.FrameMinMs` returns the length `FramePacing` would have
used anyway, and a modal frame always takes the *longer* of the two deadlines, so
nothing here can make the port run fast. It also stands down under
`KF2_FPS_LOGIC=full`. Nothing in the class reads or writes game memory; it only
ever lengthens a frame. `MenuPacing` is untouched by it — the repeat gate's six
`VSync(0)` calls present no ordering table, so they never reach the frame boundary
and this cannot see them.

**What it does not reach**, stated rather than discovered later: a counter stepped
inside a *drawing function's own body*, where no whole-function hook lands. Two are
known — stage 13's jitter accumulator at `0x8006E608` (the screen shake, which
settles faster and smaller above the tick rate) and the per-object ambient-sound
retrigger at `rec+0x40` in `func_800331B4`. Neither is an animation anyone has
reported; both need the hold/restore shape rather than a deadline, and both are
still in [TODO.md](TODO.md).

On by default and with no settings page, for the reason the menu repeat gives: a
correctness fix rather than a taste. `KF2_LOOPPACING=0` is the comparison.

### The loading screen's walking figure

**A third place with the same cause, and the one the frame boundary can never
reach.** Reported from play: *"the little man that walks across the screen on the
loading screen after pressing start on the main menu walks really fast on high
refresh rate options."*

A disc read here is a **blocking wait**, not a frame. `func_80017CA8` spins
`func_80017818(); if (flag) func_8001883C() x4;` until the CD job's queue at
`0x801B6F44` drains, and `func_800181B0` spins `func_8001883C()` while
`CdReadSync` still reports sectors outstanding. `func_8001883C` is the animator:
it draws the figure straight into VRAM — `ClearImage` (`0x800605D4`) over where
the sprite was, `MoveImage` (`0x8006069C`) where it now is — and writes exactly
three globals, which is the whole animation:

| address | what |
|---|---|
| `0x8006E5A4` | the frame counter, `++` as the last act of every call; the `& 3` and `& 7` gates that pace the sequence's sub-steps read it |
| `0x8006E5A8` | the sequence's state |
| `0x8006E5AC` | the figure's **x**, `+= 3` a call, or 5 once the state is past the middle band, while it is under 288 |

Those three are the only globals it writes (confirmed with
`find_writers.py`, and by reading every non-stack store in the function), and it
ends in `DrawSync(0); VSync(0)` — so **on hardware one call was one vblank** and
the figure walked three to five pixels every 1/60 s.

**Neither `FramePacing` nor `LoopPacing` can see this loop at all.** Both key on
the frame boundary, a `DrawOTag` after a `VSync`, and this one draws no ordering
table: measured, **zero** `DrawOTag` calls between entering the loader and the
transition fade at the end of it. So the only thing governing it is
`FrameClock`'s deliberately permissive host ceiling, `max(60, TargetFps * 2)` —
which above 60 fps barely holds a `VSync(0)` at all. The whole defect is
`MenuPacing`'s in a third place, and the shared cause is worth stating plainly:
**the game counts time in `VSync(0)` calls, and a `VSync(0)` call is no longer a
vblank.**

`patches/LoadPacing.cs` marks the window between the pre and post of
`func_80017CA8` and `func_800181B0` — depth-counted, since the second calls the
first — and a pre on GAME.EXE's `VSync` thunk (`0x8005FCC8`) holds every *blocking* call
inside it to the 60 Hz grid, exactly as the cursor repeat's six calls are held.
That restores the identity the sequence is written against, so its three counters
and the gates keyed on them are right by construction with nothing enumerated.
Measured over the area load that follows the title menu, with
`KF2_LOADPACING_PROBE=1`:

| `KF2_FPS` | off | on |
|---|---|---|
| 20 (default) | 84 steps in 1715 ms — **49.0/s** | 84 steps in 1715 ms — **49.0/s** |
| 60 | 84 steps in 856 ms — **98.1/s** | 84 in 1714 ms — **49.0/s** |
| 144 | 84 steps in 352 ms — **238.6/s** | 84 in 1714 ms — **49.0/s** |

49 a second *is* the console's number for this shape and not a coincidence: the
drain loop spends five vblanks an iteration — one in `func_80017818` and one in
each of the four animator calls — for four steps, so four steps per five vblanks
is 48. The 20 fps column is identical either way, which is the "does nothing at
or below 60" claim in one row; note that even 60 fps was **twice** too fast
before, because the ceiling there is 120.

**Capping the three counters instead was written and measured first, and it is
the worse fix.** Holding them on a 60 Hz grid leaves the wait spinning at the
render rate, so loads stay short — but it cannot reproduce the rate the port
already has at its own default: the calls arrive in bursts of four, so a cap
refuses steps that were never too early *on average*, and the default's 48.9 a
second came out at **36–40**. A fix for "too fast at 144" that also slows the one
configuration the report calls correct is the wrong trade.

**What pacing costs, stated rather than discovered later:** above 60 fps a load
takes as long as it already does at the 20 fps default — 1.7 s rather than 0.35 s
for the load measured above. That is the console's own duration and the port's own
default behaviour, and it is the direct consequence of the figure walking at the
right speed; the alternative is a loading screen whose *length* depends on the
render rate, which is the thing this port exists to stop. The window contains no
gated stage, no stage 13 and no rendered frame, so there is nothing of the
**game's** in it to slow down. 60 Hz and not `LogicHz`, for the reason
`MenuPacing.BlinkMs` gives: one vblank of an interface animation, and
`KF2_TICKRATE` is a setting about the speed of the *world*.

**It does slow something of the port's, and the same measurement is why.** Zero
`DrawOTag` in the window means no frame boundary in the window, so stretching the
window stretches the boundary gap with it. Measured at `KF2_FPS=144`, printing
the staleness of `FramePacing`'s boundary clock as each outermost wait closes:
the widest gap is **449.6 ms with pacing off and 1827.9 ms with it on**, against
a `FramePacing.BoundaryDeadMs` of 500 ms. So pacing is precisely what carries an
area load past the watchdog, at the rates the port is played at and nowhere else
— the unpaced worst case sits just under the threshold, which is why this was
never seen before.

Past the watchdog, the first gated stage reached would take `FallbackTick` and
print *"no frame boundary has been reached…"* — the alarm for the boundary being
**broken**, and the thing the boundary-loss commit exists to make loud. **That
alarm does not currently print**, in either configuration: the load ends in the
transition fade, which renders its own frames through stage 13 and so refreshes
the boundary before the main loop reaches a gated stage. That is an ordering
accident of this path rather than a guarantee — the drain wait `func_80017CA8`
is reached from six sites — so `LoadPacing.AfterWait` closes it explicitly with
`FramePacing.ExcuseBoundaryGap()`: it moves `_lastBoundaryMs` forward without
counting a frame or advancing the logic clock, so the next real boundary still
takes `AdvanceLogicClock`'s stopped-drawing branch, and a boundary that genuinely
has been lost trips the watchdog again one period later. `SpriteAnim` sees the
same gap and flips to `_onFallback` for the duration of the load, which is its
own designed behaviour rather than a defect.

**Two things about the window itself fail closed, so both are guarded.** The
pre/post pair is depth-counted, and `HookManager` runs a post only after the
recompiled body *returns* — so an `unmapped call` or a throw from a nested hook
skips `AfterWait` and leaves the depth up for the session, which paces every
`VSync` in the game to 60 Hz and quietly turns `KF2_FPS=144` into 60. A window
open longer than `WaitDeadMs` (30 s — a load's own duration with an order of
magnitude of room, not the siblings' 500 ms) is therefore dropped, once, with a
line saying so. And the pace is charged only to a `VSync` mode that **blocked**:
the window here is a whole disc wait rather than the cursor repeat's six known
calls, so a libcd poll's `VSync(-1)` counter read can reach it, and
`LibEtc.VSync` returns from that and from mode 1 without presenting — charging
them a vblank each would serialise *N* queries into *N* vblanks against
`func_800181B0`'s unbounded `CdReadSync` retry loop. The animator only ever
passes 0, so that one is a guard rather than an observed fault.

Regression-checked against `rate_matrix.py`: `walk` is bit-identical with the
patch on and off at 20 and 144, `menu-scroll` reads 6.0 and 8.5 repeats a second
with the spin at 100.6 ms and the blink capped at 16.8, and `modal-rate` at 144
reads 144.8 modal world frames a second against 22.0 loop iterations. On by
default and with no settings page. `KF2_LOADPACING=0` is the comparison.

### The flames run at the render rate

Reported from play, with a screenshot of two wall torches: *"These flames still
run really fast when you play at a high framerate. It's possible other animated
sprites/billboards are doing this, but I don't know."* They are — it is one
system and one clock word, and the guess in the second sentence is exactly right.

**These are not the animated textures.** Those are the eight scrolling slots at
`0x80192D58` that `func_8002DC78` re-uploads to VRAM, and the stage gate has held
them at the tick rate since they were found (see the sixth gated function, above).
The *picture* of those slots is `FluidSmoothing` — VRAM still changes 20 times a
second, and the shaders blend the leftover. The flames are **billboard sprites** — table 4 of the four the renderer walks:
`0x80195174`, 128 records of `0x18`, free when the `u16` at `+0x0` is `0xFFFF`.

`func_80035550` fills the table from the area's own `0x10`-byte definitions when
the area loads, and the fields that matter are all in the first six bytes:

| offset | what |
|---|---|
| `+0x0` | `u16` definition id; `0xFFFF` is a free slot |
| `+0x2` | the visibility mask the renderer ANDs `func_80032D78`'s answer with |
| `+0x3` | **how many cels** the strip has |
| `+0x4` | **the interval** — step the cel every this many counts |
| `+0x5` | **the current cel**, seeded at load to `(rand * cels) >> 15` |
| `+0x8` | the position `VECTOR`, three words |

The seed is worth noticing on its own: two torches side by side start on different
cels *on purpose*, so they do not flicker in step. It is the one piece of evidence
that these are meant to be read as independent little fires rather than as a
texture.

The second loop of `func_800331B4` draws slot `i` by handing `func_80032588` the
cel index `u8[rec+0x5] + 0x80` on the stack, and then steps it:

```c
if (u8[rec+0x4] != 0 && u32[0x80195170] % u8[rec+0x4] == 0)
    if (++u8[rec+0x5] >= u8[rec+0x3]) u8[rec+0x5] = 0;
```

and **the last act of `func_800331B4` is `++u32[0x80195170]`**. That word is the
whole clock of the system — three sites in all of `GAME.EXE`: the init zero in
`func_8002DF80`, the modulus here, the increment here — and `func_800331B4` is
called once, from stage 13. So it counts **rendered frames**, and every animated
billboard in the game burns, sparks and flickers at the render rate. Measured
standing in the save's own area: **4488 cel changes a second at `KF2_FPS=144`
against 640 at 20**, which is the ratio of the two render rates and nothing else.

#### Why the stage gate cannot reach it, and what can

This is the class `docs/TODO.md` records as *"a counter stepped inside a drawing
function's own body"*, alongside stage 13's shake accumulator at `0x8006E608` and
`func_800331B4`'s own ambient-sound retrigger at `rec+0x40`. `HookManager` detours
whole functions; `func_800331B4` **is** the renderer's world and object walk, so
skipping it draws nothing and the gate is not available.

What is available here and is not available for the other two is that the counter
is a **single word that everything divides**. Hold it and the whole system holds,
with no field enumerated and no per-slot rule — which is why this one is a fix and
those two are still open. `patches/SpriteAnim.cs` is a **hold/restore pair** around
`func_800331B4`: on a frame the world did not advance on, the pre records
`0x80195170` and the 128 cel bytes and the post puts them back. The sprite is
still *drawn* with the cel the tick left it at, because the submit happens before
the step and the step is then undone.

Three details are load-bearing:

* **The frame identity, not the tick flag alone.** A modal loop's redraw
  (`LoopPacing`) calls stage 13 again, and the transition fade renders its own
  frames from inside stage 2. Each of those is its own frame boundary, so keying on
  `FramePacing.Frames` as well as `FramePacing.TickedThisFrame` makes a second walk
  inside one frame a held one rather than a second step. `FramePacing.Frames` was
  added for this — it is an *identity*, not a rate.
* **A hold fails closed, so it needs the watchdog the stage gate has.** Not
  hypothetical: the first measured run of this patch lost the frame boundary — the
  failure `patches/recompone/0027` and `FramePacing.FallbackTick` exist for — and
  with `Frames` frozen the identity test can never pass again, so every flame in
  the game stood still for the rest of the session while the world played on at the
  right speed. A frozen picture is worse than a fast one. When `Frames` has not
  moved for 500 ms the step comes off a wall-clock grid at `LogicHz` instead, and
  the port says so once.
* **The restore is per slot and checks the id**, so an area load rewriting the
  table under us cannot put a stale cel index into a slot that now holds a
  different strip.

#### There is nothing to interpolate, and that is the whole design

Unlike the smoothing patches there is no in-between to draw: the cels are authored
frames of a fire, and a half-way flame does not exist. So this is a **rate and not
a picture**, it is on by default and it has no settings page — the class of frame
pacing and the menu repeat, not the class of dithering.

At the tick rate every frame is a tick frame, so the port's own 20 fps default is
unchanged with this on or off. Measured with `rate_matrix.py sprite-anim`, which
is the scenario written for it:

| fps | cel/s, patch on | cel/s, `KF2_SPRITEANIM=0` |
|---|---|---|
| 20 | 620.2 | 640.3 |
| 60 | 640.1 | 1861.6 |
| 144 | 646.1 | 4487.7 |

(31 live slots in that area; the count scales with how many are live, so only
compare a row against the same area.) The `stepped/s` column is the direct
statement of the fix: 20.0, 20.6, 20.8 with it on against 20.7, 60.1, 144.8 with
it off. `KF2_TICKRATE=30` at 60 fps reads 930 cel/s against the 620 that 20 Hz
gives, which is exactly 1.5x — so it follows the **world's** clock and not the
render rate. Regression-checked: `walk` reads 2845 units at 20 fps and 2844 at
144 with the patch on, and 2844 at 144 with it off.

`KF2_SPRITEANIM=0` is the comparison and `KF2_SPRITEANIM_PROBE=1` reports cel
changes a second, live slots, walks a second and how many walks stepped.

### The comparison mode

`KF2_FPS_LOGIC=full` (`patches/FullRateLogic.cs`) does the other thing: no gating
at all, and the two rate words the game re-derives every frame — walk speed
`0x80199558` and turn rate `0x8019955C` — scaled by `30 / target` on a pre hook of
`func_80028DB8`. That is the seam `mods/kf2debug`'s speed multiplier already uses,
and the scale is applied multiplicatively because by then the words carry the run
ramp, the encumbered halving and the hit-stun zeroing.

It buys turning, walking and collision genuinely sampled at the render rate, and
it is **not** a shipping mode, for reasons worth stating before anyone measures
them: pitch steps by a flat 3 and does not scale; gravity integrates
`0x8019954E` per tick and does not scale; and every per-tick counter in the game —
the death sequence, spell lifetimes, the poison tick, equipment regen, animation —
runs at the render rate.

### What it costs, and what is still unverified

Sampling the game thread in an area put 161 of 200 samples in the pacing sleep, 23
in `Present` and six in game code — about a millisecond of MIPS against thirty-two
of waiting. Drawing more often costs nothing this port has not already got.

**The default is 60 fps and a 20 Hz world**, three pictures per tick. 20 remains
the world's clock, which is the console's own arrangement. The interpolated camera
no longer swims, lags into a bounce, or jitters
against a wall — that was the extrapolation, now replaced — and smoothing ships
with the 60 fps picture.

The counters that *are* answerable. The 65-tick death clock at `0x8019951A` is the
measurement, since stage 3 bumps it once per logic tick — its slope against wall
time *is* the tick rate. Driven from `KF2_SHELL`: `kill`, then poll `state` for
`deathFrames`. Measured in area 1, slot 2:

| `KF2_FPS` | `KF2_TICKRATE` | rendered | ticks/s | 65 ticks in |
|---|---|---|---|---|
| 20 | 20 (default) | 20.5 | **20.00** | 3.20 s |
| 30 | 20 | 30.5 | **19.97** | 3.20 s |
| 60 | 20 | 60.0 | **20.06** | 3.19 s |
| 120 | 20 | 120.5 | **20.00** | 3.19 s |
| 144 | 20 | 144.5 | **20.00** | 3.20 s |
| off | 20 | 60.0 | **19.99** | 3.20 s |
| 30 | 30 | 30.5 | 29.48 | 2.16 s |
| 60 | 30 | 59.0 | 29.87 | 2.14 s |
| 144 | 30 | 144.5 | 30.01 | 2.14 s |
| 20 | 30 | 20.5 | 20.00 | 3.20 s (render-limited, see above) |

`KF2_TICKRATE=30` reproducing 2.14-2.16 s is the regression check: it is the same
2.11-2.13 s this section recorded before the tick rate moved, so the setting is
genuinely a setting and not a one-way change. Rendered rate is stage-8 calls per
second off `KF2_SMOOTH_PROBE=1`.

Walk distance is the cross-check that does not go through the death clock. A fixed
2 s hold of Up covers **1817 units at 20 Hz against 2574 at 30 Hz** — the game
moves a fixed amount per tick, so the distance scales with the tick rate and not
with the frame rate. (The ratio is 0.71 rather than exactly 2/3 because the run
ramp completes in a fixed number of ticks either way.)

Frame smoothing, at 60 fps against the 20 Hz world, turning in place:
`120/120 frames carried, mean phase 0.60 tick, yaw 20.8 u` — further per frame
than the 18.1 u recorded against a 30 Hz world, which is the longer tick showing
up. **Its probe now says which of the two reasons a frame was skipped**; it used
to print "0 of N carried (phase idle)" whether the phase was zero or the player
was simply standing still, and the second reads as a broken logic clock when it is
nothing of the kind.

The measurement to run it against is `mods/framestats`, restored for this work and
now reporting **two** histograms: vblanks per frame (how long the frame took) and
`VSync` calls per frame (how many times the game asked). Since `0021` those are
different numbers, and conflating them is what made the first pass at frame pacing
read as circular.


## Menus draw the world live

**Mechanism measured; the picture has not been looked at by eye, and a shop has
not been reached by the shell at all** — the measurements are the in-game menu,
which shares every routine below.

Reported from play: in a shop, the widescreen margin flashes black, and the
picture behind the shop has none of the enhancements (AO and the rest).

Every menu in `GAME.EXE` — the in-game menu `func_80018E80`, the shops
(`func_8001D544`, `func_8001DD34`, `func_8001DF5C`, `func_8001E2F0`), the save list
`func_8001C624`, `func_8001BE60` — runs on one framework:

| routine | what |
|---|---|
| `func_80022754` | enter: saves both primitive descriptors (to `0x8006EB24`/`0x8006EB30`), shrinks them to `0x6400` bytes each at `start`, and `StoreImage`s the displayed frame to `start + 0xC800` |
| `func_80022530` | frame head: flips `0x8017E084`, points `0x8017E0A4` and the OT pointer `0x8018E0A8` at that buffer's, `ClearOTagR(ot, 0x2000)` — the same two OTs stage 13 uses |
| `func_800226A8` | presenter: `DrawSync`, `VSync`, `PutDrawEnv`, `PutDispEnv`, **`LoadImage` of the stored frame**, `DrawOTag` |
| `func_800228C8` | leave: puts the descriptors back |

The presenter is stage 13's own presenter `func_8002E0FC` plus that one
`LoadImage`, and the `LoadImage` is the whole of both reports. It is 320×240 and
carries no depth: the margin exists only in the render target ("The margin's only
clear is the game's own" in [WIDESCREEN.md](WIDESCREEN.md)) and nothing draws it,
and AO, the Z-buffer and per-pixel lighting all need geometry the paste does not
have.

**`patches/MenuWorld.cs` replaces the presenter and redraws the world every menu
frame.** Stage 13's drawing half — `func_8002E22C(0, 0)` (the stored view),
`func_8002D3A8`, `func_80032400`, `func_80031D5C`, `func_80033E78`,
`func_80031C94`, `func_800331B4` and the four tints — runs into an ordering table
of its own; that table's terminator is written to point at the head of the menu's
table, and one `DrawOTag` walks both, so the world is under everything the menu
draws, the HUD included, exactly as the paste was. What is not called is what
advances the world or presents: stage 13's head `func_8002E064` (its three frame
counters are zeroed by hand), the tickers `func_8002DC78` and `func_80033FBC`, the
inline jitter accumulator, `func_8002E0FC`, the frame gate and `func_8003549C`.
Two holds cover what the walk steps by itself: `SpriteAnim.Hold` keeps the cels,
and a pre on `func_80014158`/`func_80013D08` keeps the ambient sources silent.

Where it writes, and why nothing else is disturbed:

- **The table and its descriptor go in the frozen frame's store**, `start + 0xC800`,
  which nothing reads once the paste is gone. The primitives go into
  `PrimBuffer`'s second buffer (`0x80264000`), or the saved world `desc1` when the
  buffers are not relocated; either way inside the range the packet records cover,
  which is what gives the pass its depth. The layout is checked at entry and a
  session that does not fit keeps the game's presenter untouched.
- **The world's GTE, `0x8018E19C` (the current model bank) and `0x8018EAA0` are
  put back to what the last stage 13 left**, and the menu's restored after. A shop's
  item preview (`func_80022CAC` → `func_8002E5E8(3, …)`, drawn by `func_8002156C`
  through `func_800346CC`) leaves bank 3 current and sets its own matrices; the HUD
  reads the bank before the tile walk selects bank 0.
- **The pass puts the world's background clear back.** `func_80022754` turns
  `isbg` (and `dfe`) off on both draw environments (`0x8018E0AC`, `+0x5C`), since
  its paste covers the whole frame every menu frame. With the paste gone nothing
  cleared the buffer: wherever the world leaves the background showing, the
  semi-transparent fog blended over whatever the buffer held two frames earlier —
  the menu — so the fog brightened towards white a frame at a time and the menu's
  panels showed through it. Reported from play in area 1 at `12276 -12800 54750`,
  unchanged by every setting. Stage 13 leaves `isbg 1, rgb 0,0,0` there; the
  renderer's post records the word at `+0x18` of each environment, and the
  presenter and `MessageFade`'s step put it on around their `PutDrawEnv` and take
  the menu's back after. Measured: both environments read `isbg 0` at the menu's
  enter before; after, `PutDrawEnv … isbg=1` on every menu frame (60 passes/s),
  and messages still fade and hold as before.
  **The picture after has not been looked at.**
- A session is decided once at `func_80022754` and ended at `func_800228C8`, at the
  main loop's stage 9, or at any overlay load.

Measured, in-game menu, area 1, 16:9, `KF2_FPS=144`, against `KF2_MENUWORLD=0`:

| | frozen copy | live world |
|---|---|---|
| primitives reaching the margin | 0.0% | 10.7% |
| triangles carrying a depth, a second | 0 | 9,840 (164 a frame, the main loop's count at the same spot) |
| menu frames / ticks a second | 60.0 / 20.0 | 60.0 / 20.0 |
| present | wide | wide |

5.3 KB of the 400 KB buffer at peak, no overflow, 60 passes a second. The menu
opens and closes repeatedly with a session each time. **Not checked**: a shop, the
save list, and whether the backdrop looks right — that it matches the frame before
the menu opened, that the sides are filled, and that nothing in it moves.

### Messages draw the world live

A sign or an NPC line is the same shape with its own loop. `func_80035B48` shrinks the
buffers the same way, `StoreImage`s the texture space it is about to use to
`start + 0xC800` (0x25800 bytes, put back when it closes) and `MoveImage`s the frame to
(320, 0); `func_800356F4(brightness, step)` then steps a fade a frame at a time —
stage 13's head `func_8002E064`, four `POLY_FT4`s (the moved frame in two halves at
colour `0x80 - b/2`, then the message picture subtractive in bucket 1 and additive in
bucket 0), `DrawSync`, stage 13's presenter `func_8002E0FC` — until the brightness
leaves 1..0x77, or until a button after all were up. It returns -1 or -2 when the fade
ran out and the brightness when a press cut it short, and `func_80035B48` reads that.

`MenuWorld.MessageFade` replaces it, loop and return value to the letter, with the
world pass in place of the two halves of the moved frame. Its table and descriptor go
after the VRAM save, at `start + 0x32000`, and its primitives in `PrimBuffer`'s second
buffer, so it needs the relocated buffers; with `KF2_PRIMBUF=1` a message keeps the
game's loop. For the one `func_8002E0FC` call a step, the OT pointer names the pass's
table, whose terminator leads into the message's.

Two things the game's loop could leave alone and a live world cannot:

- **The `MoveImage` lands on world texture pages.** The RECT at `0x8006E610` is
  (320, 0) 320×240, texture space the area's walls sample, and `func_80035B48` only
  puts it back (`LoadImage` from `*0x8017E09C`, `start + 0xC800`) at close. The game
  never drew the world while it was there; the pass does, and every wall came out as
  the frame copy read as 4- and 8-bit texels. `MessageFade` loads the saved rect
  back before its first step, since nothing it draws samples the copy; the game's
  own restore at close repeats it harmlessly.
- **Between the fades the game draws nothing.** Once the fade-in runs out,
  `func_80035B48` spins on `PadRead` until a button, with no `VSync` and no draw, so
  the wide target went idle and the present fell back to the 1x VRAM frame a few
  seconds in — the message "resolving to 4:3". `MessageFade` waits there itself,
  drawing the world at the last brightness, and returns `0x50`, the value the
  caller's own wait sets on the press, so the fade-out starts where it would have.
  A press that cuts the fade-in short returns its brightness as before.

Measured after both, `(3, 0)`: 768 frames held live until Cross, 60 passes/s,
`[present] wide ~175, vram fallback 0` every 2 s throughout, then the 7-frame
fade-out and close. Reported from play before the fix: garbled wall textures behind
every dialogue and a drop to 4:3 after a few seconds; after it, confirmed from play.

The dim cannot stay a colour on the frame, because nothing is pasted any more:

- **A message `MessageText` has decoded** draws no quads at all. The overlay lays black
  at `(b/2)/0x80` over the whole picture and then its box and text — exactly the paste's
  `0x80 - b/2`, and over the margin too.
- **Any other** (the place-name cards) keeps the game's two text quads, over a black
  `POLY_F4` at 50% once the brightness is half up. The console has no multiply, so 50%
  (mode 0 against black) stands in for the 58% the paste reaches.

Measured, 16:9, `KF2_FPS=144`, `KF2_MESSAGETEXT_TEST=6:120,3:0`: both fade in over ten
frames of the live world, 18.3% of primitives reaching the margin and about 160
depth-carrying triangles a frame, the same as the frame before; the card kept the
game's picture and `(3, 0)` was drawn by the overlay; each stays up until the button,
as with the game's loop (`KF2_MENUWORLD=0` for the comparison). **Not looked at.**

## Drawing message text

**An experiment, off by default (`KF2_MESSAGETEXT=1`; there is no control in the
window). It shipped on after v0.3.0 and was judged by eye in play and turned back
off:** only the ~81% of messages that decode completely are replaced, so play mixes
the game's own lettering with interface-font text on an opaque modern box from one
sign to the next, and the replacement does not look like the game. See "Judged" at
the end of this section.

A sign or an NPC line is a 4-bit TIM in `TALK.T` or `ITEM.T` ("Full-screen messages
are pictures" in [GAME_INTERNALS.md](GAME_INTERNALS.md)), so the text can be no
sharper than 256×256 at 16 colours. `patches/MessageText.cs` reads the text back out
of that picture and draws it itself:

1. **Decode.** A pre on `func_80035684` — the TIM is in RAM and not yet uploaded, and
   `func_80035B48`'s pre has recorded `(file, entry)` — cuts the image into 8×14 cells
   from x = 4, lines on a 14-pixel grid whose phase is 3 or 10 (the text is centred on
   odd and even line counts), thresholds each cell at half the brightest ink colour,
   and looks up the FNV-1a hash of its 14 row bytes in `patches/MessageGlyphs.cs`. One
   unknown cell and the message is left to the game.
2. **Hide.** On a full decode the TIM's palette is zeroed in RAM. Colour `0x0000` is
   transparent to a textured primitive, so `func_800356F4`'s two text quads draw
   nothing and nothing in the game's code had to be replaced.
3. **Draw.** An ImGui panel over the game picture draws an opaque box over the text's
   rows and each line at its own cell position, in the interface font at 12.5 game
   pixels, in the line's brightest palette colour. Opacity follows `func_800356F4`'s
   brightness, which it keeps in `s2` and which a pre on stage 13's presenter
   `func_8002E0FC` reads while the fade runs.

**The glyph table carries no text.** `scripts/msg_glyphs.py` builds it from the disc:
it cuts every image the same way, reads each with tesseract, lines each OCR line up
against the line's non-empty cells when the counts agree, and takes the majority vote
per cell. OCR errors are outvoted — the single image tesseract read as `equi pment` and
`ie.` decodes as `equipment` and `die.` Measured: 111 glyphs; 551 of the 585 images
on the grid decode completely; the other 94 of the 679 are a second, bolder font, the
centred place-name cards and box drawing, and stay pictures. `--dump DIR` writes the
decoded text for proofreading, and is not to be committed.

`KF2_MESSAGETEXT_TEST=3:0,6:360` opens those messages through `func_80035B48` from
the main loop's stage 9, ten seconds in, with `KF2_AUTOPAD` to dismiss them — the
command channel cannot reach an NPC. Measured, 16:9 at 144 fps: `(3, 0)`, `(6, 360)`
and `(6, 240)` decoded to the same text as the offline dump, `(6, 120)` (a place-name
card) was left to the game, and the panel drew 126 frames of `(3, 0)` at full opacity.

**Judged** (2026-09-22, from play): the replaced messages read as an Arial-like font
on a very modern box and are not wanted, and the mix with the undecoded ones (which
stay the game's picture) is worse than either alone. Anything that comes back here
would have to draw in the game's own glyphs, and at that point it adds nothing the
TIM does not already show. Placement and the fade were never judged. Behind it is the live world
when `MenuWorld` is on ("Messages draw the world live"), and the dim is then this
panel's; with it off, the game's 1x `MoveImage` copy.

## Auto reload

`patches/AutoReload.cs` reloads the last save when the player dies. King's Field
has no retry; without it a death costs the menu, then the load screen, then the
slot. (It began as `mods/autoreload` and became a patch — see "Auto reload became
a patch" below.)

It is small because it adds **no loading path of its own**. Both halves were
already in the game and both are written up in
[GAME_INTERNALS.md](GAME_INTERNALS.md): the death latch under "The character's
stats are buf2", and the in-game load sequence under "The game can load a save
without leaving the area". This is the wiring between them.

**One post-hook on `game` at `0x8002A550`** — the end of main-loop stage 3. Not `func_80029CBC`, which is where the game does its own load from:
that is dispatched only from the state machine's arms for states 1 and 2, so it
stops being called the moment the state byte latches to `0x11`. Stage 3 itself
runs every frame, dead or alive, because the death sequence *is* one of its arms.

Per frame it reads one byte, `0x801994E1`. Not `0x11` and it clears its arming
and returns, so a live player pays a single read for the whole mod. On the
edge into `0x11` it additionally requires `HP == 0`, which is what separates a
death from any other reason that byte could hold `0x11`, then starts a clock. The
reload fires once, after a configurable delay (default 2 s) during which the
game's own death sequence plays.

### The game is already reloading, and it wins the race

The thing that nearly sank this, and the reason the delay is not just a sleep.
**Death is a timeline, not a state**, clocked by a `u16` frame counter at
`0x8019951A`. `func_8002A264` zeroes it, state `0x11`'s handler increments it,
and those three sites are its only uses in `GAME.EXE`:

| counter | what the handler does |
|---|---|
| 1–31 | the death animation |
| 32–64 | fade to black, amount `(n − 32) << 7` |
| **65** | `func_80024154(0, 0, 0, 0, 0, 0xFF)` |

That last call is **the same area-entry routine the patch calls, with area 0** —
the game's own respawn, and what "you died, start again" actually is. (The branch
above it, at `0x8002AFBC`, is the resurrection-item path: a literal position and
yaw `0x0C00` into area 1. It is the debug-looking warp already noted at that
address.)

65 frames is **2.17 s at 30 ticks a second** — and the default delay was 2.0 s.
Five frames of margin. The first run reloaded correctly and the second went back
to the beginning of the game, from identical code and an identical log line, which
is exactly what a race that tight looks like. They are logic ticks rather than
rendered frames, so the margin follows `KF2_TICKRATE` and not `KF2_FPS`: at the
20 Hz default the same 65 ticks is 3.25 s, which is more margin, not less — but
the race is what the hold at frame 31 exists to remove, and it removes it at any
rate.

So it **holds the counter at 31** while it waits. The animation finishes,
the fade never starts, the respawn never comes due, and the delay becomes ours
rather than a bet against the game's clock. The reload logs the counter it fired
at for exactly this reason: `held at frame 31` is the assertion, and anything
near 65 would mean the hold had failed.

The reload is `func_80023638(slot)` and then `0x80029E0C`'s arm transcribed, with
a `c.SP -= 0x20` window for `func_80024154`'s fifth and sixth arguments — MIPS
passes those on the caller's frame at `sp+0x10` and `sp+0x14`. `c.Snapshot()` /
`c.Restore()` bracket the whole thing so the hooked function's caller sees the
registers it left.

Two details that are the patch's and not the game's:

- **`func_80029E5C` afterwards, but only if the state byte survived.** The game
  reaches that arm from a live state and so never has to clear the death latch;
  coming from `0x11`, something must.
- **The slot.** `0x8006E5D4` is the game's own record and is what "last used"
  means, but it is zero until a save or a load has run. Dying on a fresh New Game
  therefore has nothing to reload, and it logs once and leaves the death alone
  rather than inventing a slot. A fixed slot can be pinned in the settings.

`KF2_AUTORELOAD` and `KF2_AUTORELOAD_SLOT` mirror the two settings, which persist
to `interface.ini` under `kf2.autoreload.*`. **The delay is the third env var and
is no longer a setting**: `KF2_AUTORELOAD_DELAY` still moves it, but the slider is
gone and `kf2.autoreload.delay` is not read any more. It decides only whether the
death reads as a death before the screen changes, and both ends of the 0-10 s
range it offered were wrong — at 0 the reload lands inside the death animation and
reads as a glitch, and at 10 the player has had time to reach for the menu this
patch exists to save them from. 2 s is what shipped, and what every measurement of
the death clock at `0x8019951A` is taken against. Retiring the key rather than
leaving it read is the rule the map's nine went by: a key still read after its
control is gone strands whoever set it.

**Dying on demand is the hard part of testing this**, so the settings page has a
*Simulate death* button: it zeroes HP and calls `func_8002A264(0)`, which is
exactly what the damage path does. It refuses when max HP is zero, since buf2 is
clear until an area is up.

**Status: working on the attract demo, which dies on its own and is therefore a
free test rig** — leave the port running and it will kill the character for you.
Three consecutive deaths in one session each reloaded slot 2 into area 1 at
`HP 46/86, LV 6`, `held at frame 31` every time, with no `unmapped call` and no
exception.

`fdat02` in the overlay log is the tell for the failure: the game's own respawn
loads it, so a run that reloads correctly never does. With the setting off it
appears right after the death, and with it on it does not appear at all.

Still wanted from a real session: a death in a *different* area from the save,
which is the case that makes `func_80024154` re-enter a `fdat` module other than
the resident one, and a save written mid-session to confirm `0x8006E5D4` tracks
saves as well as loads.

### Auto reload became a patch, and the port grew a Gameplay tab

The reason is the one under "What belongs in a mod, and what does not": a mod is
something a player can reasonably be without, and *four screens of menu after
every death* is not a taste. A player who installs this port expects the retry to
be there, and a mod defaults to off and loads silently when disabled — the failure
mode is a death that just costs the menu, with nothing said. So `mods/autoreload`
is now `patches/AutoReload.cs`, on by default.

The port has done this conversion three times now (dither, frame pacing, this) and
the mechanical part is settled:

- `[PostHook("game", Address = …)]` becomes an explicit `SymbolRegistry.Resolve` +
  `HookManager.AddPost` under a `ModInfo` the patch declares for itself, attached
  on the **first `OverlayLoadedEvent`** — that is the earliest moment every
  overlay resolves, and hooks cannot be added once the game is past the loads.
- `IMod.OnLoad`'s config read becomes a `RuntimeReadyEvent` listener, because
  `ConfigManager.Load()` runs after `Program.cs`. The env vars move up into
  `Configure(…)` called from `Program.cs`, and each one sets a `…FromEnv` flag so
  the saved value cannot overwrite it — the same precedence `FramePacing` keeps.
- `IMod.DrawSettings` becomes an `IPatchPage`; instance fields become static
  properties with `Set…` methods, so the page reads live state instead of a copy.
- `OnUnload` has nowhere to go and needs nowhere: a patch is never unloaded, and
  this one never wrote game memory it had to put back.

**Where the page went is the part that was not mechanical.** The frame rate and
the dither switch extend the runtime's `display` section because they genuinely
are video options. Auto reload is not: it is a rule about what happens when you
die. The runtime's five sections are all about the machine — interface, input,
display, paths, audio — and none of them is that, so the port adds a sixth.

`ISettingsSection` is public and `SettingsRegistry.Register` replaces by id, so a
**new section needs no patch to the checkout** any more than renaming one did.
`patches/settings/GameplaySection.cs` is `Id = "gameplay"`, `Order = 7` (between
Video's 5 and Audio's 10, so the runtime's own order is untouched and the tab
sits among them rather than after Paths) and a `Draw()` that does nothing at all:
`SettingsPopup` draws a section's own content and *then* its extensions, so the
pages registered against `"gameplay"` are the entire pane. Registration happens in
`PatchSettings.RegisterUi`, on `RuntimeReadyEvent` — after `HostWindow`'s `Load`
has registered the five, before the popup is first drawn.

One difference from the `display` rename: `settings.gameplay` is a **new** key, so
`Localization.Merge` has to supply all three languages. An override of an existing
key can leave the others alone, since what is already there is what you wanted; a
new key with only `en` behind it makes pt-BR and es-419 warn on every lookup and
then show the key itself.

Verified in a run: the section list came back
`-10:interface | 0:input | 5:display=Video | 7:gameplay=Gameplay | 10:audio | 20:paths`,
and *Simulate death* from the new tab logged
`reloaded slot 2 into area 1 (HP 46/86, LV 6, held at frame 31)` — the same
result the mod gave, so the hook, the config read and the reload path all survived
the move.

## A dynamic map

`patches/Map.cs`, `patches/MapMarkers.cs`, `patches/MapRender.cs`,
`patches/MapFullscreen.cs`, `patches/MapPanel.cs`, `patches/MapOverlay.cs`,
`patches/settings/MapPage.cs`.

    KF2_MAP=0             the whole feature off (on by default)
    KF2_MAP_MINIMAP=1     the corner minimap on (off, and not a setting)
    KF2_MAP_PAUSE=0       the world keeps running under the full map (not a setting)
    KF2_MAP_FLOOR=lower   pin a stacked half instead of following the player
    KF2_MAP_ARROW=1       the player as an arrow with a heading, not a dot
    KF2_MAP_MARKERS=1     the marker layer on (off, and not a setting)
    KF2_MAP_STYLE=blueprint  the port's original blueprint plan (not a setting)
    KF2_MAP_SHADE=1       colour each tile by its height byte (off, not a setting)
    KF2_MAP_WALLS=1       tint the tiles whose +4 bit 0x80 is set (off, not a setting)
    KF2_MAP_PROBE=1       dump the 80x80 grid as ASCII, its occupied extent and a
                          marker census, and open both maps

King's Field is a maze, the original shipped no automap, and until now everything
in this port that knew where you were was a debug instrument — `kf2debug`'s Warp
tab, `KF2_SHELL`'s `state`, the `[KF2-AGENT]` beacon. A patch rather than a mod
for auto reload's reason: it is a thing the port itself should offer, so it
should not be able to be absent or silently disabled, and its knobs go under
Gameplay beside it. **The pad's touchpad button opens the full-screen map**, `M`
does the same from the keyboard, `N` toggles the corner minimap, and `Shift+M`
opens the docked panel with the per-tile readout.

### The floor plan is already in RAM, and it is not polygon soup

**Confirmed.** The area loader `func_8001689C` copies `0x3E80` words — 64,000
bytes — to `0x801C8484`, and 0x600 more to the collision-shape block at
`0x801D8484` (`generated/game.cs:4231-4239`). That first block is an **80 x 80
grid of 10-byte tile records**, `tile = 0x801C8484 + 800*z + 10*x`, which is the
resolution `func_80031B1C` performs before drawing each tile. A tile spans 2048
world units, so `tileX = worldX >> 11`. The 24x24 grid at `0x80192EAC` that
`CullCone` and `CullGrid` work on is only a per-frame *visibility window* over
this map — see "The map is an 80x80 tile grid" in `docs/GAME_INTERNALS.md` for
the record's five fields and the two stacked halves.

So a map is a **read**. No hook, nothing written to game memory, and with both
viewports closed it costs one bool test a frame.

### The invariant that proves the whole chain

**Confirmed, and it is the check worth keeping.** The player has to be standing
on a half the renderer draws, and that half's floor Y — `-(heightByte << 7)` —
has to equal the player's own Y at `0x801994F0`. `KF2_MAP_PROBE=1` prints it on
every area load:

    map: player tile 35,36 on the upper floor, drawn, height byte 116
         -> floor Y -14848 against player Y -14848 (gap 0)

Measured **gap 0** across three save slots, two areas and both floors. One line
confirms the tile addressing, the 10-byte stride, the 5-byte half layout, the
height byte at `+1`, the half selector at `0x801D9C8E` and `tileX = worldX >> 11`
together — which is why the probe prints it rather than just the ASCII dump.

### The arrow's angle is derivable, and the map was the view from below

**Confirmed twice, from the code and from a live walk.** `func_80028080` is what a
walk step goes through, and it adds `-sin(yaw) * d` to the X at `0x801994EC` and
`+cos(yaw) * d` to the Z at `0x801994F4` (`generated/game.cs:26043-26071`;
`func_8005EB08` is odd so it is sine, `func_8005EC10` takes `|a0|` so it is
cosine). The yaw it reads is the **base** angle at `0x8019950E`, as a signed
short. So the heading on the ground is `(-sin yaw, cos yaw)`: yaw 0 faces +Z and
yaw 0x400 faces -X.

The attract demo confirms that much without a hand on the keyboard, which is what
it is for. Over two seconds of steady heading:

| samples | measured delta | normalised | `(-sin yaw, cos yaw)` |
|---|---|---|---|
| yaw 959 -> 959 | dx -2315, dz +217 | (-0.9957, 0.0933) | (-0.9951, 0.0994) |
| yaw -> 717 | dx -848, dz +432 | (-0.891, 0.454) | (-0.891, 0.454) |

`AgentBeacon.Snapshot()` gained a `yaw` field for this, so `KF2_SHELL`'s `state`
answers heading as well as position. It reads the *composed* angle at
`0x80199506` rather than the base, because that is the one the renderer uses and
so the one the arrow should agree with; the two differ only by the frame's own
deltas.

**What that measurement cannot see is which way up the map is**, and the first
version had it upside down. Screen X was world X and screen Y was world Z, so the
arrow was `(cos, sin)` of `yaw + pi/2` and the screen angle *increased* with yaw —
self-consistent, and consistent with the walk, because a mirrored map and a
mirrored arrow agree with each other about everything except handedness. Reported
from play: **turning left swung the arrow right.**

The world's up is **-Y** — a half's floor is `-(height << 7)`, which is the one
reading the probe prints every load — so a viewer above the plane looks along
**+Y**, and with +X to the right that puts **+Z at the top of the screen**. Laying
screen Y out along +Z is therefore the view from underneath. It also settles the
handedness the passage above was arguing about: `up x forward` is
`(-Y) x (+Z) = -X`, so yaw increasing really does turn you left, and on a map
drawn from above the arrow's screen angle must **decrease** with yaw:
`(cos, sin)` of `-(yaw + pi/2)`.

So the fix is in the map, not in the arrow. `Map.RowF(worldZ) = Span - TileF(z)`
is the screen row a world Z draws on and `Map.RowOf` converts a row back to a
tile; `MapRender.Draw` takes `z0..z1` as **rows** and both viewports place their
origin from `RowF`, which leaves every screen-space calculation they already had
alone. Two details are easy to get wrong: the sub-tile fraction runs backwards
with the flip, so it is `Span - TileF` and not `Span - 1 - TileF`, and the hover
readout has to run `RowOf` on the row it floors out of the cursor or it names a
tile mirrored about the middle of the area.

**Negating the arrow alone would have matched the report and been wrong**: it
would have squared the arrow with the turn and left it pointing across the
direction of travel, since the mirror was in the map. That is the general shape —
a mirror is invisible to any measurement taken entirely inside the mirrored frame,
and only the two facts from *outside* it (the world's up, and a player saying
which way they turned) can find it.

The ASCII dump `KF2_MAP_PROBE=1` writes is deliberately **not** flipped: it is a
dump of the grid in memory, row `z` on line `z`, and its job is to say whether the
copy and the stride are right. It is upside down with respect to the drawn map.

### A blank grid reads as a full one

**Confirmed, and it is the trap in the occupancy test.** The renderer draws a half
whose model index is below 240, and a *zeroed* record's index is 0 — so between
areas, when the game has cleared the block, all 12,800 halves pass the test and
the map draws a solid rectangle over the whole area. Measured "12800 occupied
halves, height 0..0" at the boot, against 5,126 and 6,313 in the two real areas.

`Map.Ready` is the test: every half occupied at exactly one height is not
something a real area can be. The panels draw nothing while it is false.

### The area loader looks like the right hook and is not

**Confirmed, and it was tried.** The map is stale for up to a copy period after an
area load: `OverlayLoadedEvent` fires, the area byte at `0x8017E060` has already
moved, the next draw re-copies — but `func_8001689C` has not yet run its
`0x3E80`-word copy, so the grid is still the area you just left. Measured: the
probe printing a player 12,800 units off the floor the map was reading, on a tile
the map called empty.

`func_8001689C` is where that copy lives, so a post-hook on it looks like the
exact "the grid is now the new area's" signal. **It is called every frame** — the
function is 1,716 bytes with the load in a branch, and the hook fired **5,673
times in 40 seconds at 144 fps**, turning a four-a-second grid copy into a
per-frame one and dumping the probe 5,673 times. Reverted.

What is there instead costs nothing: the copy is cheap and self-corrects within
the 250 ms period, and the *probe's* dump waits for the standing-on-a-drawn-half
invariant above rather than firing on the area byte. The map's own stale window is
a fraction of a second of loading screen, which nobody sees.

### Where it draws, and what that decided

A host ImGui surface, not PSX primitives in the game's own frame. The in-frame
route exists — `GpuPrims.Tri/Quad/Sprite` — but `GpuPrims.SetOrderingTable` is
**dormant**, nothing in the tree calls it, and `patches/Widescreen.cs` already
owns the only `Replace` on `DrawOTag` on all three overlays. Left for a later
pass.

**Every read happens inside the panels' own `Draw()`, and that is correct rather
than lazy.** `LibEtc.VSync` calls `Runtime.PresentFrame` -> `HostWindow.Present`
-> `PanelManager.DrawPanels`, all on one thread, so a panel draws *inside the
game's own VSync call* and sees exactly the memory the game left there. There is
no snapshot handoff to get wrong. The 64,000-byte copy is still taken only four
times a second, because the architecture it carries — the drawbridge and the
minecart are tiles, not models — does not move faster than that.

The full map is an `IPanel`; the minimap is an `IFloatingPanel`, which is the
interface for a panel that should not count towards the dockspace's layout. The
minimap's `IsOpen` **is** its feature switch, because `PanelManager.DrawPanels`
only calls `Draw` on an open panel — that is what keeps it off the title screen
and out of a load. Panels do not auto-populate the menu bar, and neither map
declares an entry there: the top bar is the port's own chrome, and a *Game*
heading beside System, Mods and Debug reads as part of the machine rather than
part of King's Field. Both are reached the way a player reaches them — `M` and
the pad's touchpad button for the full-screen one, `Shift+M` for the docked
instrument. The two panel titles still supply all three of the runtime's
languages, since `Localization.T` prints the key itself for one it has never
heard of.

**An overlay is a z-order problem before it is a drawing one, and the minimap
shipped invisible because of it.** Reported from play as "there is no way to
overlay the map — only the map as a whole tab works": the minimap window carried
`ImGuiWindowFlags.NoBringToFrontOnFocus`, which reads like chrome politely
declining to steal focus and is nothing of the kind. ImGui reads that flag **once,
in `CreateNewWindow`**, and pushes such a window to the *front of `g.Windows`* —
which is the **back** of the display order — instead of appending it. The window
also has `NoInputs` and `NoFocusOnAppearing`, so nothing ever focuses it and it
has no second route forward, and `OutputPanel.Draw` pushes an **opaque** black
`WindowBg` over the whole dockspace. So the minimap was drawn correctly on every
frame, underneath the game picture, for the life of the feature. Dropping the flag
appends it instead: it sits above everything that already exists, the dock tree
cannot displace it (`##DockHost` carries the flag itself, so focusing a docked
panel does not bring the tree forward), and a popup created later still covers it,
which is what should happen. The full map never saw any of this because it is an
ordinary docked panel — **the split in the report is the diagnosis**, and
`MapOverlay` is the only `IFloatingPanel` in the tree, so nothing else had ever
exercised the combination.

What is still true of the anchoring is that the minimap is pinned to the *main
viewport's* work area rather than to the game picture, and the picture is
letterboxed inside `OutputPanel`'s content region. At 4:3 in a wide window a
corner therefore lands on the black bar beside the game rather than over it. That
is arguably the better place for it and it is the user's to judge; anchoring to
the image would need the runtime to publish that rectangle, since `OutputPanel` is
`internal`.

**Five anchors, and the numbering is the old bitmask plus one.** `0..3` were read
as a mask — bit 0 the right edge, bit 1 the bottom — which is exactly two binary
choices and therefore cannot express a *centred* one. `4` is top centre, and
`MapOverlay` switches on the value instead of masking it; the four original
numbers keep their meanings because they are already written in players'
`interface.ini`, so "Top centre" is appended to the combo rather than slotted in
beside the other two top entries. An unrecognised value falls back to the shipped
top-right rather than landing off-screen.

**The edge gap was a setting** (`kf2.map.minimap.pad`, 0-200 px, 12 — the
constant that shipped, and the value the field now keeps; the control and the key
went with the minimap, see "What the Map page is down to"). It is scaled by
`Theme.Scale` alongside the size, so
the gap keeps its proportion as the interface scale moves rather than closing up
on a large one, and it is clamped where it is *used* as well as where it is set:
it is a saved number, and a big enough one would push the map off the screen
leaving no control on it to bring it back. A centred anchor spends it on the top
edge only — there is no horizontal edge to stand off from.

#### The minimap's shape and opacity

Two knobs that were under Gameplay ▸ Map, both pinned to the picture that shipped
— a fully opaque square — for the rule the rest of the port follows: a picture
nobody has judged by eye does not become the default. That same rule is why they
are no longer controls at all: see "What the Map page is down to".

**Opacity** (`kf2.map.minimap.opacity`, 0.15-1) is the ground and the tiles, not
the window. The window's own background is a rectangle ImGui fills before the draw
list runs, so it can be neither round nor faded independently of what is drawn over
it; the overlay carries `NoBackground` instead and fills its own ground, which
makes the setting the single store for how solid the map is. The palette is built
once as packed `IM_COL32` words, so fading is `MapRender.Fade` scaling the high
byte — a colour that is already translucent (the wall tint, the lit tint) stays
proportionally so. **The player's arrow is exempt**: an overlay you can see the
game through is the point, and a "you are here" marker you cannot find is not.

**Shape** (`kf2.map.minimap.shape`, square or circle) is a clipping problem with no
clipping available. ImGui's clip rects are rectangles and a draw list cannot erase
what it has already drawn, so the usual mask — a ring painted in the background
colour over the corners — needs the background to be opaque, which is exactly what
the opacity setting forbids. So the circle is cut per tile instead
(`MapRender.ClipToCircle`): each tile's rect is clamped to the chords the disc
allows **at its far edges**, which can fall short of the circle by a fraction of a
tile but can never spill past it — and spill is the artefact a drawn border ring
makes obvious. The edge is therefore scalloped by up to one cell at the diagonals;
at the default 220 px over 25 tiles a cell is 8.8 px.

Measured cost: 144.0 fps drawn and 20.0 ticks/s with both viewports drawing at
`KF2_FPS=144`, indistinguishable from `KF2_MAP=0`.

### The full-screen map, and the button the PS1 never had

`patches/MapFullscreen.cs`. Reported from play: *"the minimap is a cool feature,
but it contrasts hard with the game design."* The two viewports that existed were
a corner overlay and a **docked ImGui panel** — a title bar, a toolbar, a zoom
slider, a hover readout printing ten hex bytes — which is a debugger over a
320x240 picture, not a map you open with a controller in your hands. So there is a
third viewport: the whole area, over the dimmed game, no chrome, opened and closed
with one button.

It is a third *viewport* rather than a third map. The palette, the tiles, the
markers and the player are `MapRender`'s, the reading is `Map.Refresh`'s, and this
class is a rectangle, a scale and a scrim — the same relationship the minimap has.
It carries `NoInputs`, so the mouse still reaches the game and the menus behind
it, and there is nothing on it to click: everything it draws is decided by the
settings the other two already share. The minimap closes while it is up; a corner
map over a map of the same area is two answers to one question.

**The touchpad is reachable because it is not a PS1 button.** `InputManager`
dispatches `ControllerEvent` straight off SDL, `ev.Cbutton.Button` and all,
*before* `PadState` maps anything onto the PSX pad — so listening there gets the
raw `SDL_GameControllerButton`, of which the DualSense's touchpad click is **20**
(SDL 2.0.14 and up, through the PS5 driver). Nothing downstream has a slot for it,
so it cannot leak into the game as a keypress, and the pad's sixteen real buttons
are left entirely to the game. The event bus only dispatches `ControllerEvent`
while something listens, so the listener is also what turns that dispatch on.

A pad whose SDL mapping has no touchpad simply never sends button 20, and
**nothing here can ask it whether it has one** — `InputManager` keeps the
`GameController*` to itself, so `GameControllerHasButton` is out of reach. The
binding is therefore a setting rather than a probe: **Input ▸ Map ▸** "Open the
full-screen map with" (`patches/settings/MapButtonPage.cs` — it was on the Map
page under Gameplay until a binding was judged to belong with the bindings),
offering Touchpad (the default), L3, R3, Select and None,
stored as the SDL index in `kf2.map.pad.button`. `Device` is deliberately not
filtered — it is SDL's joystick *instance id*, not a player number, so any pad
opens the map.

**It fits the area rather than following the player.** `Map.Copy` gained the
occupied box per half (`Map.Extents`), computed in the same pass as the height
range and printed by the probe, and the view is scaled to that box and centred
on it — so nothing moves as you walk except the dot, and a route can be read
off a stable picture. That was expected to be a fraction of the grid and
**measured as the whole of it**: areas 0 and 1 both run `x 0..79, z 0..79` on
both halves. Fitting the extent is therefore fitting the 80x80 grid.

**A 900p window used to show less of the maze than a 1440p one, and the floor
on the scale is why.** A tile below 6 logical px, multiplied by `Theme.Scale`,
abandoned the fit and centred on the player's **tile** — the same quantisation
the dot has — so the picture stepped a square at a time rather than sliding.
That is a readability bargain, and it is the wrong one for a map. At a 1.5
interface scale a 4:3 900p picture has about 8 px a tile to fit the grid and
the floor asked for 9, so the view cropped to the rooms around the player;
the same scale on a 1440p picture still fitted. Reported from play as less
map visible at 900p than at 1440p. The floor is gone: the occupied extent is
always in view, tiles shrink with the window, and the ceiling of 22 logical
px a tile is kept so a one-room area does not fill the screen. **Never judged
by eye**: whether a few pixels a tile at 900p still reads as a plan.

#### "Full screen" is the game picture, not the window

Reported from play: *"the fullscreen map conforms to the window and not the draw
area, so it has a fairly odd appearance."* It did, and the reason is that the game
picture is **not** the window and nothing outside the runtime's `OutputPanel` knew
where it was. The picture is an ImGui `Image` inside that panel, fitted to the
panel's content region at the display's aspect (`FitAspect`) and centred in it, so
three things eat into it before a pixel of game is drawn: the main menu bar and
the dockspace border take a strip off the top and the edges, a docked panel takes
its share of the rest, and a window whose shape does not match the display's
leaves a bar on two sides — 4:3 in a 16:9 window is a bar either side.

Sized to `viewport.Size`, the map covered all of that. Its scrim dimmed the port's
own chrome as well as the game, its floor plan was centred on the *window* rather
than on the picture, and its edges lined up with neither: a window over the port
rather than a screen the game had put up.

`patches/recompone/0029` publishes the rectangle from the one place that computes
it — a public `OutputView` set in `OutputPanel.Draw`, from `GetCursorScreenPos`
and the fitted size, immediately before `ImGui.Image`. Ordering needs no care:
`PanelManager.DrawPanels` walks its list in registration order, `HostWindow`
registers the Output panel during initialisation and `Program.cs` registers the
map viewports long afterwards, so the rectangle is always this frame's.

`MapRender.Picture` is the accessor, and it **falls back to the viewport's work
area** when `OutputView.Valid` is false — before the first frame is presented, and
while the panel is collapsed. Fitting nothing would be a blank map; fitting the
window is what the map did already, so the fallback degrades to the old behaviour
rather than to a defect.

One consequence needed handling: the picture is now the smaller of the two
rectangles, so a fixed 24 px margin and a two-line header at a large `Theme.Scale`
would eat the map they frame. Both are capped at a share of the rectangle (5% and
10% of its height), which changes nothing at a full-window picture.

Measured with all three viewports drawing at `KF2_FPS=144`: **144.0 fps drawn,
20.0 ticks/s**, and no throw over two area loads and an autostart. **Never judged
by eye**: whether the map now reads as part of the game, which is the whole point
of the change. The corner minimap still anchors to the viewport's work area rather
than to the picture, so it can sit over a letterbox bar — the same defect one
viewport along, deliberately left alone until someone looks at the full map.

### You are here, approximately

`MapRender.DrawPlayerDot`, and it is now the only one a player gets
(`KF2_MAP_ARROW=1` is the comparison). It was the default of a two-entry combo;
the argument below is about the game rather than about the player's screen, which
is what took the combo away — see "What the Map page is down to".

The arrow told the player two things the game never told them: where in the room
they stand, to the world unit, and which way they face, to a twelfth of a degree.
That is a satellite fix in a game whose whole difficulty is a maze you are meant
to be lost in, and it is what "contrasts hard with the game design" meant. A dot
centred in the occupied tile says only *you are in this square* — which is what
someone drawing this maze on graph paper would have known.

So it is drawn on the **tile**, not on the position: the world coordinate goes
through `Map.TileOf` and back out as `tile + 0.5` across and
`Map.RowOf(tile) + 0.5` down, which is the same row arithmetic the tile fills use,
so the dot lands in the square the fill drew rather than half a cell off it. It
steps a square at a time as you walk. The radius follows the **cell** rather than
the caller's size, because occupying the square is the point: a dot at the
minimap's four or five pixels a tile, a filled square's worth on the full map.
Full opacity, for the reason the arrow had it — the minimap's opacity exists so
the ground stops hiding the game, and a "you are here" you cannot find is not
worth drawing.

The arrow is kept rather than deleted: what it records about the game's heading is
measured (`func_80028080`, and the mirror above it), and `KF2_MAP_ARROW=1` keeps
that live rather than turning it into archaeology.

### The map the game itself draws

`Map.Style`, `MapRender.DrawNative`, `MapRender.Frame`,
`MapRender.DrawPlayerPointer`. **The game's own map is the only one a player
gets**; the blueprint the port shipped with survives on the console as
`KF2_MAP_STYLE=blueprint`. See "Five map controls that were not choices" below for
why that stopped being a combo.

The first version of this map was accurate and did not belong to the game. It
filled every walkable tile pale blue-grey on near-black, ruled a faint grid over
it, put a yellow dot in it and left the whole thing floating over a dimmed
picture. Everything in that sentence is a decision an instrument makes: it is the
docked panel's palette, scaled up. Reported from play as not conforming to the
game's own styles, against a capture of the map item King's Field II actually
hands the player.

**What the original does is the one difference that matters: it does not fill.**
Its board is one flat slate green, and the plan on it is the *wall* between a
walkable tile and a void one, inked in a green so dark it reads as black. Room
interiors and the unexplored field are the same colour. So a corridor is two
parallel lines rather than a bright ribbon, and the picture reads as something
drawn on paper by somebody walking the maze — which is what a map in a game with
no automap is — instead of as a photograph of it from above.

Everything else is sampled rather than chosen. From a capture of the in-game map:

| | |
|---|---|
| field | `#3A523A` — the board, and the void alike |
| ink | `#0E200E` — the plan |
| under | `#2D3A2D` — the mottled shading inside it |
| frame | `#7B8C7F` at the bevel's highlight, over `#43433A` |
| player | `#E7C4C5` body, `#F78272` core |

The frame is three concentric rectangles reproducing a bevel measured across the
original's edge (dark outside, brightening to the highlight, dropping to a grey
shadow at the slate), width a share of the board rather than a pixel count, and
**drawn inside the rectangle it is given** — both callers hand it the edge of
something already clipped, so a bevel painted outside would be cut away on the
two edges a minimap is pinned to.

Four consequences follow from copying the original rather than recolouring the
port's map:

- **The board is square**, because the game's is. A plan stretched across a 16:9
  picture reads as a widescreen HUD however green it is.
- **The scrim drops from 0.93 to 0.55.** The game's map is a board held up in
  front of you, drawn into the world with the dungeon lit around it; a black-out
  behind it is the one thing that would still read as an emulator overlay after
  everything else matched. The board itself stays opaque, so nothing is harder to
  read — only the surround is visible. It is black rather than the map's own
  ground: a green wash over the dungeon would read as a filter on the game.
- **There is no grid**, at any scale. The `grid` argument is simply not consulted.
- **The player is the original's marker** — a pale **pointer** with a salmon boss
  at its middle, drawn on the tile so it steps a square at a time, and **turned to
  the cardinal direction you face**. It is traced off the marker in the game's own
  map rather than invented: a long thin blade through the tile, a short crossbar
  at the centre, a filled salmon square on that centre with a pale pip in it and
  short salmon arms running down the blade, and a third of the way forward a
  **two-step chevron** — a plate the height of the crossbar with a narrower one
  the height of the boss stepping out in front of it, which is what a pixel
  triangle looks like at this size. Every dimension is a share of the marker's
  length, taken from the capture at 63 px: blade 5/63 thick and running -0.45 to
  0.53 (the capture's own forward bias — it is the *boss* that sits on the tile's
  centre), crossbar 26/63 across, boss 16/63 square, its arms 27/63 long, pip 4/63
  square, chevron plates at 0.29-0.36 and 0.29-0.44 with half-widths 0.21 and
  0.13. **The chevron is the only asymmetric thing in it**, which is exactly why
  the first version — a plain cross — said nothing about facing until it was
  rotated and then read as a cross rather than as a pointer once it was.
  Quantising to a quarter is the arrow's bargain read the other way round: the
  objection to the arrow was never that a heading is shown, it was a heading good
  to a twelfth of a degree on top of a sub-tile position. A quarter turn is
  "north, roughly" — what someone standing in a corridor with a paper map knows,
  and the piece that tells you which way to hold the page without telling you
  where in the room you are. It is also the only rotation that leaves a marker
  built out of axis-aligned rectangles axis-aligned: the snap is taken in the
  game's own angle units (`q = round(yaw / (Turn/4)) mod 4`) and the rotation
  comes off a four-entry cosine table, so the bars stay exactly square rather than
  a float sine's worth off it; `MapRender.Cardinals` is 4, and 8 would cost only
  that alignment. `PlayerMark == 1` still draws the arrow, recoloured into the
  palette and unchanged in what it claims.

Two things in the picture are readings rather than measurements, and are marked as
such. The **mottled darker shapes** in the original are reproduced as *the other
stacked half* — a square with something drawn on the floor you are not standing on
is washed in `under` before the plan goes over it. Nothing here proves that is
what the game shaded; it is the only thing in the tile record that produces shapes
of that kind, and it is information the blueprint style throws away. And the
**height ramp** is kept, in the board's own green at about a fifth of the
blueprint's contrast, because a room interior that reads as a fill is exactly what
this style undoes.

An edge is drawn by the tile that is *visible*, on each side whose neighbour is
not, so a shared wall is inked once and no seam is doubled — and the neighbour
test asks the 80x80 grid rather than the drawing window, so a plan does not sprout
a border along the edge of whatever rectangle a caller happened to ask for. Fog
counts as opaque there, so a half-walked area is inked at its frontier and reads
as a plan rather than as a shape with a torn edge.

The blueprint is kept rather than deleted: it is the picture the fog, the extents
and the marker layer were all judged against.

**Never looked at by eye:** any of it. The palette is sampled, the geometry is
arithmetic, and whether the result actually reads as the game's map — at
full-screen scale, at minimap scale, with fog on, with markers on — is the user's
call.

### Opening the map stops the world

`Map.Pause`, on and **not a setting** (`KF2_MAP_PAUSE=0` is the comparison). It
had a checkbox; the paragraph below is the whole argument and it does not have two
sides, which is why — see "What the Map page is down to".

The full-screen map is the whole area over a dimmed picture with no chrome and no
input — a screen you stop to read — and reading it takes as long as it takes.
King's Field's own map was an *item*, used from a menu that already stopped the
game; this one is opened with a button in the middle of a corridor, so without a
pause the port is asking the player to plan a route while something walks up
behind them. The minimap and the docked panel deliberately do not pause: a
heads-up corner map is meant to be read while walking, and the instrument is for
watching the game run.

**The mechanism is the stage gate held shut, not a new one.** `FramePacing` already
declines to run the six gated stages on a frame the logic clock did not tick, and
those six stages *are* the per-tick world — the object state machine, the pad read
and player movement, the entity table, the effect lifetimes, the area module's own
logic, the fade and the texture scroll. A pause is that decision taken on every
frame instead of on three in four. So:

- **Nothing is written to game memory and nothing is restored.** A pause that ends
  leaves the game exactly where the last tick put it; there is no state to get
  wrong and nothing to unwind if the process dies with the map open.
- **Stage 13 is not gated, so the picture keeps being drawn** at the render rate,
  and the map is laid over a live frame rather than over a still the port would
  have to keep re-presenting itself.
- **Every counter in the game stops together**, because they all hang off those
  stages. There is no list of timers to remember.
- **The pad is not read**, since the read is in stage 3. That is what a pause is,
  and it is also why the command channel's `map` verb is on the fast queue: the
  heavy queue drains on stage 3.

`FramePacing.PauseWhen(predicate)` is the entry point, and it is a *predicate*
rather than a flag deliberately. The panel closes by more than one route — `M`,
the pad button, and the Gameplay switch that turns the map off — and a latch
missed by one of them is a game that never resumes. It is read once a frame, at the two places that already
mean "a new frame" (the frame boundary, and `FallbackTick` when the boundary has
been lost), because host input is polled from inside the game's own `VSync`
(patches/recompone/0007) and a predicate read per stage could otherwise run half a
tick — a state machine stepped against an entity table that was not. It is gated
on `Map.InGame` as well as on the panel, since the map can be opened at the title
screen, where it draws nothing and where pausing would freeze the intro.

**A modal loop needs the other half, and `LoopPacing` is where it goes.** A gated
stage can simply be skipped; a modal loop — a transition fade, a message box, a
cutscene — *is* the main loop, on the stack, presenting its own frames, and no gate
can reach its body. `LoopPacing` already holds such a loop to one iteration per
tick by filling the gap with redraws; a pause is that fill not ending. Two changes
make it one: the fill is entered when paused even at the tick rate (where
`Extrapolating` is false and it would otherwise return at once), and while paused
it is **not subject to the redraw cap** — capped, a message box would advance
itself a couple of times a second through a pause. It cannot spin, because each
redraw passes the frame boundary, is paced by `FramePacing.Floor`, and presents
through the game's own `VSync`, which is also where host input is polled: the map
is drawn on every one of those frames and the key that closes it is read on them.
Pause redraws are counted separately from fill redraws (`held` against `n`) so the
cap warning keeps its meaning — it fired once, spuriously, on the frame a paused
map was closed inside the fill.

**What the pause does not stop.** The music and ambient audio, which is the usual
behaviour for a map screen. `MapFog` keeps sampling on the vblank, harmlessly
re-writing cells it already has. `func_800331B4`'s per-object ambient-sound
retrigger at `rec+0x40` is stepped inside a drawing function's own body and is
outside every gate in the port, pause included — the same exception the frame
pacing notes record.

**Measured** at `KF2_FPS=144`, area 1, through the command channel's `map` verb:
20.0 ticks/s before, **0.0 ticks/s** for the whole time the map was up, 20.0 again
on closing it, with the picture holding 144.0 fps throughout. The world's phase
holds at 0 while paused, so the frame the pause holds is a tick-aligned one — a
pose the game produced, not an interpolated one. Repeated three times with the
pause landing *inside* a warp's transition fade (areas 5, 2 and 7): the fade froze,
resumed cleanly on closing the map, and no redraw-cap warning fired. A heavy shell
command issued while paused times out after five seconds saying stage 3 never ran,
which is the pause seen from the other side.

**Never looked at by eye:** whether a frozen frame under the scrim reads as
paused or as a stall, and whether the map is quick enough to open and close that
pausing is what a player wants rather than an interruption.

### What is measured and what is not

**Measured:** the grid address, stride and half layout; the height byte; the half
selector; the heading; the per-area occupancy and its bounding box (the whole
80x80 grid, both halves, in areas 0 and 1); that all three viewports draw for
45 seconds without throwing; that they cost no frame rate.

**Not looked at by eye, and the user's to judge:** whether the floor plan looks
like the area you are standing in — which is what decides whether occupancy should
come from the model index at `+0` (what it uses, because that is the renderer's own
test) or from the collision bytes at `+2` and `+3`; whether the height shading
reads or muddies; whether bit `0x80` of `+4` is the wall it behaves like in the
visibility flood or the "see through" `docs/WIDESCREEN.md` calls it; and whether
the minimap's size, corner, range, shape and opacity are usable in play — the
scalloped edge of the circle and the readability of a faded map over a dark area
in particular. On the full-screen map: whether the fitted scale is readable in a
large area at a 900p window (a few pixels a tile, which is now how the whole
maze stays on the board), whether the scrim is dark enough to read tiles over a
bright scene, and whether a whole-area view is what a player wants rather than
one centred on themselves. On the dot: whether it reads at minimap size, and whether losing the
heading costs more than the honesty buys — and, on the native cross, whether a
quarter turn is coarse enough to keep that bargain and fine enough to be worth
having, and whether the blade reads as the pointing end at a minimap's scale. **On the native style, everything**:
the palette is sampled off a capture and the geometry is arithmetic, but whether
an outlined plan reads at a minimap's four pixels a tile, whether the 0.55 scrim
is dark enough behind a bright scene, and whether the other-floor wash reproduces
the original's mottling or merely resembles it, are all pictures. **And the pad button is unverified
here** — no controller was connected to any of these runs, so that SDL sends
button 20 for a DualSense touchpad click is read off SDL's mapping rather than
measured; if it does not arrive, the same setting offers L3 and R3. The docked
panel's hover readout prints all ten bytes of the tile under the cursor for
exactly that reason.

### Fog of war

`patches/MapFog.cs`, and the seam it fills is the one the map shipped with.

    KF2_MAP_FOG=0         fog off for the run (on by default)
    KF2_MAP_FOG_LOS=0     the line-of-sight gate off (on by default)
    KF2_MAP_FOG_PROBE=1   a line a second: tiles seen, lit, refused, records, flushes
    KF2_MAP_FOG_PROBE=2   also the raw 24x24 grid, the gate's verdict and its walls

The map used to reveal the whole area. It now draws only the tiles the player has
seen, remembered **per save slot** and kept between sessions in a file beside the
memory card. On by default: the mechanism below is
measured and the picture has been judged for shipping.

**No new reverse engineering was needed and none was done.** `func_8002D3A8`
rebuilds the 24x24 grid at `0x80192EAC` at the head of every frame — the 4:3
frustum flattened onto the map, scanline-filled and then flooded for occlusion.
Fog is that grid ORed into an 80x80 bitset, on the mapping the game's own queries
use: `cell = (world >> 11) + Offset`, so the world tile of cell (0,0) is the
negated pair at `0x80192EA0`/`0xA4`.

#### The grid is a culling test, and a culling test may over-report

**The sentence that stood here said that grid "*is* what the player can see", and
it is not.** It is what the frame might have to draw, and the two differ in one
direction only. The flood (`patches/CullGrid.cs`'s `Cell`, a transcription of
`func_8002CFC8` / `func_8002D15C`) marches rings out of the eye and lights a cell
when **either** of its two parents on the ring inside it is lit. Two parents ORed
is a 45-degree spread per ring, so one lit cell at the mouth of a corridor
illuminates an expanding wedge behind the wall beside it. For the renderer that
costs a few polygons nobody sees; for a store that never forgets it paints rooms
the player has never been able to see into, which is what "the map reveals places
disconnected from the room I am in" is.

So a lit cell is checked against the floor plan before it is written: **recursive
symmetric shadowcasting** (eight octants, the standard Bergstrom form) out of the
player's own tile over the 80x80 map, a tile opaque when it carries no drawn model
(`+0 >= 240`, the renderer's own test — the gaps between rooms *are* the walls in
this game). A tile is revealed only when the game lit it **and** a line exists.
Shadowcasting rather than a Bresenham ray per cell because a ray between tile
centres clips the corners off a doorway and would under-reveal, and because it is
symmetric: the tiles the player's tile can see are the tiles that can see it,
which is the property that makes "I have been able to look at this square"
defensible.

**Both stacked halves are cast, and a cell is answered by the one its own bit
names.** Asking the game which floor the player is on is the wrong question and
two attempts at it failed measurably. The selector at `0x801D9C8E` — what
`patches/Map.cs` draws from — has its own invariant *failing* in area 5, where it
says upper and the player is 4200 units above that floor; a cast over the wrong
half reads the whole area as wall, and it did: 86 of 93 lit cells refused, the
player's own tile among them. Deriving the half from the player's Y instead only
moved the failure, leaving four of the eight areas with no floor within a storey
of the player and so no cast at all. The grid answers it per cell and for free —
bit 0 is "lit on the lower half", bit 1 "lit on the upper" — so both halves are
cast and each bit is checked against its own. Two casts cost two 53x53 snapshots
on a tile step and nothing on a sample.

The cast depends on the player's tile and the map alone, not on where the camera
points, so it is taken once per tile step and re-taken every 250 ms (a drawbridge
and a minecart are tiles). **A window with no wall in it is an unloaded map rather
than an open field** — `patches/Map.cs`'s cleared-grid trap the other way up, since
a zeroed model index is 0 and 0 is a *drawn* tile — so that half is passed rather
than refused. The gate fails open, back to the raw cone, never to a blank map.

Measured over a walk through all eight areas at 144 fps: **3993 of 8469 lit cells
refused**, the player's own tile revealed on every sample, nothing falling outside
the cast window, and 144.0 fps at 20.0 ticks/s with the minimap open. The picture
`KF2_MAP_FOG_PROBE=2` prints is the argument — the grid, the gate's verdict and
the walls it read, over the same 24 cells:

    [KF2] fog grid z27: ..........33333.........  |          xxxxx         |  |..##...###...#######....|
    [KF2] fog grid z29: .........33333333.......  |         xxx#####       |  |#############.......##..|
    [KF2] fog grid z33: ........333322222233....  |        ############    |  |#########..............#|
    [KF2] fog grid z36: .......333222222223333..  |       #############xx  |  |########...........#...#|

The player stands in row 36. The flood lit a wedge running up to row 23; the wall
column shows a solid mass across rows 29 to 33, and the corridor beyond it is the
`x` run — lit by the game, five tiles the far side of a wall, and refused. What no
counter can say is whether the revealed shape now matches where the player walked.

Its switch was *Only what you could see*, under Gameplay ▸ Map beside the fog
one, because turning it off and walking the same room again is the one comparison
a player can make by eye in a single session. That is a comparison rather than a
preference — the ungated fog writes a permanent lie into a store that never
forgets — so it is on, the key is retired, and the comparison lives on the docked
panel's *Sight* tick and `KF2_MAP_FOG_LOS=0`.

#### A cell byte is not a boolean, and "nonzero" over-reveals by 7x

**This is the finding, and the note this section replaced had it wrong.** It said
"a cell is visible iff its byte is nonzero", which is what `patches/CullGrid.cs`'s
grid comparison and `patches/CullCone.cs`'s ring census both test — correct for
*them*, since they are asking whether two builds of the same grid agree. It is not
what visible means. The scanline fill writes the marker over the **whole
trapezoid** before the flood runs, the flood then clears the marker on the cells
it can prove are occluded, and what stays behind in those cells is the flood's own
working state. So nonzero is the frustum's footprint, not its visible set.

The bits are the two stacked floor halves: `func_80031B1C` draws half A on bit 0
of the flags byte and half B on bit 1, the flood's `_marker` is bit 0 on the lower
half and bit 1 on the upper (it reads the half selector at `0x801D9C8E`), and the
other of those two bits is the ray-alive flag it carries along a row. Everything
above them is bookkeeping — the `0xC0` the stock epilogue forces over the 3x3
around the eye, which `patches/CullCone.cs` also ORs over its near-camera rescue
discs. **`byte & 3` is the test**, and `KF2_MAP_FOG_PROBE=2` is what settled it,
by printing the array rather than arguing from the flood's source:

    [KF2] fog grid z 16: ..+++++22+++++++++++++..
    [KF2] fog grid z 17: ...++++22++++++++++++...
    [KF2] fog grid z 18: ....++++222222++++++....
    [KF2] fog grid z 23: ........++22+++.........

The `+` is the trapezoid, the `2`s are the corridor the player is actually looking
down, and the player's own tile is one of them. Measured standing still: **190
cells nonzero against 26 lit** — and a trapezoid nine tiles wide at ten and a half
deep cannot hold more than about 110 tiles in the first place, which is the
arithmetic that says 190 was never a visible set.

#### The sampling clock is the vblank, so there is no hook

The accumulator has to run with the map closed, so it cannot live in a panel's
`Draw` the way every other read in `patches/Map.cs` does. The obvious seam is a
post on `func_8002D3A8` — and that is the fallback — but `VSyncEvent` costs **no
hook at all** and, since `patches/recompone/0021`, fires on a wall-clock 60 Hz grid
rather than per rendered frame: 60 samples a second at 20 fps and at 144 fps
alike. The grid is stable for the whole frame once built, so a vblank read gets a
complete one. At the 20 fps default each grid is sampled three times, which the OR
makes free; at 144 fps 60 of the 144 grids are sampled, and a cone 72 degrees wide
would need the player to turn 72 degrees in 16 ms to leave a gap.

#### Three guards, and two of them were written after being measured

* **The camera must be within two tiles of the player.** The area byte at
  `0x8017E060` moves when a save's area is unpacked while the grid in RAM is still
  the last frame of the area being left — the same window `Map`'s 250 ms re-copy
  self-corrects through, except that fog *accumulates*, so one bad sample is a
  permanent lie. Also a 250 ms hold after any overlay load. A cutscene camera
  somewhere else stops accumulation, which is the conservative way round.
* **The slot and area bytes must be bytes the game means** (slots 0..3, areas
  0..10). Measured: a record for **"area 99"** written while a reload was
  unpacking `buf0`, on a frame whose HP and camera both still read as a live area.
* **A sample whose cone drew nothing is not a view of anywhere.** Between a New
  Game and its area being placed, HP is up and the player and camera both read
  0,0, so everything above passes; without this the "always reveal the tile the
  player stands on" rule wrote tile 0,0 into whatever record the area byte named.

#### Identity: the game's slot byte, and a scratch bucket for slot 0

A record is keyed `(slot, area)`. The slot is the game's own at `0x8006E5D4`, which
both the load `func_80023638` and the save `func_80023764` write and which is
**zero until one of them has run** — so a New Game accumulates into a scratch
bucket, and the moment the byte becomes 1..3 the scratch is ORed into that slot's
records and dropped (measured: `merged 1 unsaved record(s) into slot 2` on the
autostart path, which New Games and then loads over it). The consequence to know is
that the identity is the *slot* rather than the save: starting a New Game over
slot 2 inherits slot 2's old fog, and loading an old save shows everything that
slot has explored since.

#### The file

Beside the memory card — `Path.ChangeExtension(CardAPath, ".fog")`, so `carda.fog`
by default — because that is where this game's saves are and what the slot in the
key refers to. Resolved once on `RuntimeReadyEvent`, since `ConfigManager.Load`
runs inside `HostWindow.Initialize` and so after `Program.cs`; a card path changed
mid-session is not followed.

    offset  size  what
    0       7     "KF2FOG\0"
    7       1     version = 1
    8       2     u16 record count
    10      per record: u8 slot, u8 area, 800 bytes (bit = z*80 + x, LSB first)

802 bytes a record, so a finished game on every slot is about 29 KB. Written whole
through a temp file and `File.Move(overwrite: true)`, so a kill mid-write leaves
the old store rather than half a new one; a bad magic, version or length loads as
empty rather than throwing. **There is no app-exit event** — `Runtime.Shutdown`
dispatches nothing — so the writes that matter are the periodic one (10 s, only
when dirty), the one on a `(slot, area)` change and the one on `OverlayLoadedEvent`,
with `AppDomain.ProcessExit` and a `finally` around `Entry.Run` catching a clean
exit and a crash respectively.

#### What it draws

The seam widened from `Func<int,int,bool>` to `Func<int,int,int>`, because the
third state costs nothing: the accumulator has the frame's grid in its hand, so
**0 unexplored / 1 remembered / 2 in view right now** is free, and the last state
draws a pale wash over the tile so the map carries a live cone. `MapFog.Predicate`
is what both viewports pass, and it is `null` — draw everything — both while fog is
off *and* until the first sample lands, because a fog map with no record yet is a
blank rectangle and reads as the feature being broken.

Fog is per **tile**, not per floor half: the visibility grid has no notion of the
two stacked halves, so walking a corridor reveals the tile on both floors of the
map.

#### What is measured and what is not

**Measured:** the byte test, from the array itself; separate records per area
across `warp 3` and back with no bleeding; the scratch merge; persistence across a
kill and a restart (`2 record(s) in carda.fog`, and the area's total carrying on
from where it stopped); the file's own arithmetic (`10 + 802 * count`); and the
cost — **144.0 fps drawn and 20.0 ticks/s** at `KF2_FPS=144` with the minimap open,
which is what the map measured before fog existed.

**Not looked at by eye, and the user's to judge:** whether the revealed shape
matches where they walked, whether the in-view wash reads or distracts, and whether
the whole-tile reveal is noticeable on a map that draws one half at a time. The
settings page has *Forget this area* and *Reveal
this area* for exactly that comparison.

### What is *in* the area: the marker layer

`patches/MapMarkers.cs`. The tile grid is the half of a map that does not move;
the other half is everything standing in it, and that is not in the grid at all.
It is in the **four world tables `func_800331B4` draws from**, which are already
written down — "`func_80032588` is fed from *four* tables" in
[GAME_INTERNALS.md](GAME_INTERNALS.md), and the same four rows
`patches/ObjectSmoothing.cs` carries and `AgentServer`'s `nearby` reports. This is
a third reader of a measured fact rather than a new address hunt:

| # | base | stride | count | drawn when | position | rotation | drawn as |
|---|---|---|---|---|---|---|---|
| 1 | `0x8016C544` | `0x7C` | 200 | `u8[+0x9] == 1` | `+0x2C` | `+0x40` | red triangle |
| 2 | `0x80177714` | `0x44` | 396 | `u16[+0x6] != 0xFF` | `+0x14` | `+0x24` | blue square |
| 3 | `0x8019CC6C` | `0x48` | 128 | `u8[+0x0] != 0xFF` | `+0x14` | `+0x24` | purple diamond |
| 4 | `0x80195174` | `0x18` | 128 | `u16[+0x0] != 0xFFFF` | `+0x8` | none | amber dot |

**The liveness test is the renderer's, not the owning stage's**, for the reason
`ObjectSmoothing` learned the hard way: an object is *drawn* on `+0x6` and
*stepped* on `+0x4`, a creature is drawn on `u8[+0x9] == 1` and stepped on
`u8[+0x0] != 0xFF`, and the two are opposite ways round. A map should show what is
on screen, so it uses the predicate the screen uses.

**A shape as well as a colour**, because the minimap draws a marker at four or
five pixels and a colour alone does not separate at that size. Billboards default
off — a torch-lit corridor holds dozens and they would bury the markers a player
is looking for — and everything else is on. Markers are exempt from the minimap's
opacity, the same exemption the player's arrow has: the setting exists so the
*ground* stops hiding the game, and a creature you cannot see is what an overlay
map is for.

#### The object table outlives its area, and this is the trap

**Measured, and the guard is in.** The renderer's `u16[+0x6] != 0xFF` is right
*for the renderer*, because the renderer runs after the loader has filled the
table. A layer sampling on its own 50 ms clock has no such guarantee. Across an
area change the object table reads **258 slots drawn and 0 stepped**, carrying the
previous area's positions verbatim — slot 2 at `141311,-12800,28416` in both — so
the loader clears the behaviour byte at `+0x4` and leaves `+0x6` and the `VECTOR`
where they were. The sample is therefore **held** while not one slot passes
`+0x4 != 0xFF`, which is the shape `Map.Ready` already has for the tile grid, and
the probe says so in as many words when it fires in that window.

In a settled area the two tests still disagree — measured in area 0, **258 drawn
against 139 stepped** — and that difference is *kept* rather than filtered, since
a slot the renderer draws is a thing on screen. `Marker.Stepped` carries it and
the full map's hover readout labels the other 119 `static`.

#### Which floor a marker is on is derived

A tile record holds two stacked halves and the map draws one at a time, so a
marker takes the drawn half whose floor `-(height << 7)` is nearest its own Y.
That is the map's own founding equality — the player stands on a half the renderer
draws and that half's floor Y *equals* their Y, measured gap 0 — applied to
everything else in the area.

#### Fog of war, with the stricter rule for what moves

A tile you merely remember does not tell you what is standing in it now. So a
creature or an effect is drawn only where the tile is lit in the last sample
(state 2), while a prop or a billboard — architecture in all but name — is drawn
wherever the tile is at least remembered (state 1). With fog off the predicate is
`null` and everything shows.

#### What a marker is called is deliberately not claimed

The object table's type byte at `+0x4` dispatches through a 224-entry jump table
with thirty distinct arms, and nothing in this repo pairs an arm with a noun — so
a chest, a door and a lever are all "object" and the readout prints the raw type
and definition index instead. The one identity that *is* confirmed is the
creature's: `u8[+0x2]` is the index into the per-area descriptor block at
`0x80172624` (120-byte records; `patches/HitGuard.cs` and `docs/TODO.md` #14), so
two markers of a kind carry the same number.

`KF2_MAP_PROBE=1` is the instrument for closing that gap: it censuses all four
tables — **every** class, whatever the settings show, or a census would report
zero sprites and read as the billboard table being wrong — and prints a histogram
of the type byte. Area 0, measured:

    map markers: 0 creatures, 258 objects, 0 effects, 5 sprites
    map markers: of the objects, 139 are stepped by stage 2 and 119 are drawn but never stepped
    map markers: object types: 02x2 03x3 04x8 05x5 08x25 09x53 0Fx2 10x3 12x1
                               16x2 1Fx5 51x4 54x5 5Fx1 A0x1 E0x15 F0x4 FFx119

Pairing `09` (53 records) or `E0` (15) with what is actually on screen, area by
area, is what would turn this class into named ones.

#### Except the save point, which the game names itself

The one object identity the map does claim comes out of the game's own **use
handler**, `func_800489FC` — the routine the action button runs. It scans for the
objects in front of the player (`func_80036EC8` over the object table at
`0x80177714`), resolves each one's **definition** the way stage 2 does —
`0x80175914 + u16[rec+0x6] * 0x18`, the `0x18`-stride table of object kinds — and
dispatches on the *kind* byte at `def+0x0` rather than on the behaviour byte at
`rec+0x4` that the 224-entry jump table uses. Its **`0x0E` arm** at `0x80048FEC`
is three calls:

```c
func_800492B8(*(u8*)0x8017E060);  /* pack the 200-slot entity table into the save buffer */
func_80029C50();
func_8001C624();                  /* the slot menu -> func_8001C88C -> func_80023764 */
```

`func_80023764` is the routine that writes the memory card ("The card code is all
in GAME.EXE" in [GAME_INTERNALS.md](GAME_INTERNALS.md)), and no other arm of the
handler reaches it — while `func_8001C624` has **exactly one call site in all of
GAME.EXE**, which is this arm, so the implication runs both ways. **Kind `0x0E` is
a save point**, read off the dispatch
rather than guessed from a histogram, and that is the one marker on the map drawn
as a letter: an **S** on the tile it stands in, which is what "the save room"
means to someone reading a plan.

Three properties follow from it being a *kind* rather than a tile flag or a room:
the mark is per object, so an area with two save points gets two letters; it is
bounds-checked against the `0x1E00` bytes between the definition table's base and
the object table (320 records), because the index is game data; and the layer is
**independent of the marker layer entirely** — not merely of the object class
inside it. `MapMarkers.Saves`, on always and not a setting, which is what that
independence became once the marker layer went off by default.

**That independence is two lines and both of them had to be found the hard way.**
Shipped first, the S was independent of `Objects` only: `Scan` admitted a save
point whether or not the prop class was wanted, and the checkbox sat in the
indented block under "show what is in the area". Reported from play as *no save
points ever show up on the map*, and the reason is one line above all of that —
`Refresh` opens with `if (!Enabled) { _count = 0; … return; }`, so a player with
the marker layer off has an **empty sample** and nothing downstream can draw from
it. Measured with the census below: `live 0 … markers False`. Turning that layer
off is most of the reason the switch exists — the object squares are the clutter
— so the population that wanted a save point marked was exactly the population
that could not get one. `Shown` now folds `Enabled` into itself and `Refresh`
stands down only on `!Enabled && !Saves`; the checkbox moved out of the indent.

The other half of that report was a **test that could not fail**:
`DrawSave` clamped the glyph up to nine pixels and then asked whether it had
reached nine, so the documented fallback to an ordinary marker was dead code. It
gates on the **cell** now — below five pixels a tile, the minimap gets a square
instead — which is the number the caller actually varies.
`MapRender.DrawSave` needs room for a letter and says so: below nine pixels it
returns `false` and the marker falls back to the ordinary object square, which is
the minimap's usual answer and the full map's never. `KF2_MAP_PROBE=1` counts
them beside the type histogram and prints their tiles:

    map markers: 2 save point(s) — definition kind 0x0E in the 0x18-stride table
                 at 0x80175914: (79,41 def E6) (65,13 def E6)
    map markers: 4 save point(s) — … (40,74u def E6) (27,1u def E6)
                 (48,79u def E6) (36,36u def E6)

Two in area 0 and four in save slot 2's area, **all six carrying the same
definition index `0xE6`** — which is what a table of *kinds* shared between
instances should look like, and is the same fact read from the other side.

#### The letter goes in the middle of the room, not on the object

A save point stands against a wall like any other prop, so an S drawn on its tile
marks the corner of a chamber rather than the chamber. `Map.RoomCentre` puts it
in the middle instead, and **a room here is a definition rather than a reading**:
nothing in the grid records where one ends and the next begins, so it is the
**largest solid rectangle of drawn tiles containing the object's tile**.

A flood fill is the obvious answer and is the wrong one — every walkable tile in
an area connects to every other through the doorways, so a fill returns the whole
floor and its centroid is a point in the middle of the map. A rectangle is bounded
by walls on all four sides at once, so it stops at a doorway the way a person's
idea of a room does, and it degrades sensibly: a save point in a corridor gets a
long thin rectangle whose centre is still in the corridor beside it. Openness is
`Map.Drawn` — the renderer's own test, the one `MapFog`'s shadowcast treats as
opaque, where the gaps between rooms *are* the walls. Ties go to the rectangle
whose centre is nearest the tile asked about.

It is memoised per tile and **cleared on an area change rather than on a grid
copy**: the copy runs four times a second for the handful of tiles that move (the
drawbridge, the minecart), and clearing there would pay for the search sixteen
times a second to answer the same question. Two save points that resolve to one
centre are lettered once, since two S's on a point draw as one heavier S. The
letter is **white** in both styles — the one mark on the map that is not the
game's own, the original having no save points on it, so it is allowed to be the
one thing outside the board's palette; the plan's own ink is what haloes it.

**The letter is sized to the room as well as placed in it.** A cell is 6 to 22
pixels, so a letter one cell tall is a speck on a full map of a large area — it
was, and was reported as hard to see. `RoomCentre` therefore also returns the
rectangle's **short side**, which is how much room a mark at the centre has in
its worst direction, and the S spans `0.55` of it clamped to 2.0-3.4 tiles: three
tiles of a six-tile chamber, still two in a corridor, where two tiles of overlap
is a wall either side and the S is the thing meant to be read. The halo's offset
grows with it, or a big letter loses the outline that keeps it off the pale end
of the height ramp.

Measured: the letters sit a mean **2.2 tiles** from their objects in save slot 2's
area, so the rooms found are real ones rather than the object's own square, and
draw at **26.4 px** on a 12 px cell against 12.6 px before, at 165 fps unchanged.

`Map.Probe` makes the render side count as well, since a save point can be
sampled and still not drawn — the sample holds both floors and a viewport shows
one, which no census of the tables can see:

    map saves: 4 of 4 lettered on the upper half at 12.0 px a tile,
               2.2 tiles from the object to its room's centre, letter 26.4 px

Measured after the fix, with the marker layer still switched off. The count is of
the **shown floor**: a save point on the half the map is not displaying is
filtered like every other marker, so `0 of 2` is a correct reading rather than a
failure.

**Never looked at by eye:** whether an S lands where the game actually lets you
save. The dispatch is read out of the emitted C# and the address arithmetic is
stage 2's own, but nobody has stood on a save point and checked the letter is
under them.

#### What is measured and what is not

**Measured:** the four tables read and their counts move with the area; the
stale-table guard firing exactly in the reload window and naming itself; five
billboards in area 0 confirming table 4 is addressed right; and the cost —
**144.0 fps drawn and 20.0 ticks/s** at `KF2_FPS=144` with the full map and the
minimap both open and drawing markers, which is what the map measured before the
layer existed.

**Not looked at by eye, and the user's to judge:** whether the markers read at
minimap size, whether four classes is too many at once, whether the 119
drawn-but-never-stepped objects are things a player recognises or clutter, and
whether the creature *facing* spoke points the way the creature does — that last
is derived from the rotation the renderer builds (`s16` at `+2` of the triple,
biased `0x800`) and has never been checked, which is why it defaults off.

## Five map controls that were not choices

The Map page under Gameplay carried eleven controls and now carries six. Five
went, and the argument is the one the Video page had already been through
("Three checkboxes that were not choices" in `docs/WIDESCREEN.md`): a setting is
for a thing a player can reasonably want either way, and each of these was
instead a question the port is better placed to answer, offered as a tick.

**Style.** The whole case for `DrawNative` is that the map *belongs* — same
board, same ink, same frame as the map item the game itself hands you. Offering
the blueprint beside it as the other half of a preference asks the player to
settle what the port is for, and the answer is not a taste. So `Map.Style` is
`StyleNative` and the combo is gone. The blueprint stays in the code: it is the
picture the fog, the extents and the marker layer were all judged against, and
whatever has to be judged next will want it. `KF2_MAP_STYLE=blueprint`.

**Shade by height** and **mark sight-blocking tiles.** Both are the blueprint's
instrumentation showing through onto a board that is one flat slate green. The
height ramp was kept in the native style at a fifth of the blueprint's contrast,
which is either invisible or wrong; the wall tint reads a bit (`+4` bit `0x80`)
whose *meaning is an open question* — the widescreen notes read it as "see
through" rather than as a wall, and nothing here has settled it. A layer nobody
can say the meaning of is an instrument, not a map. Both off: `KF2_MAP_SHADE=1`,
`KF2_MAP_WALLS=1`, and the docked `MapPanel` keeps a session tick for each,
because that panel *is* the instrument.

**Show what is in the area** — the marker layer, and its four class ticks with
it. **Off.** A live read of the four tables the renderer walks tells you where
every creature, prop, spell and torch is standing, right now, through walls.
King's Field's difficulty is not knowing what is round the corner; the map exists
because the maze is a maze, not because the bestiary is a secret being kept
badly. That is the same argument the player dot won over the arrow, one layer
out. `KF2_MAP_MARKERS=1` and the panel's Markers tick are the comparisons.

**Save points.** **On, and the exception rather than a survivor.** The one object
in a maze a player wants a map to find is the one they can save at, the game
names it itself (definition kind `0x0E`), and it is a fixed feature of the
architecture rather than a live reading of what is walking about. It draws
whatever the marker layer says — which is the independence found the hard way
above, and is now load-bearing rather than a courtesy, since the layer it is
independent of is off for everybody.

**None of the five reads its saved key any more**, and that is the part worth
copying from the Video page: a key still read after its control is gone strands
whoever set it — `interface.ini` says one thing, the window says nothing, and
there is no control left to put it back with. `kf2.map.style`,
`kf2.map.shade`, `kf2.map.walls`, `kf2.map.markers` and its five relatives are
retired. The panel's ticks are deliberately session-only for the same reason.

What the page keeps is the map itself, the pad button that opens it, whether it
pauses, how the player is drawn, the minimap with its own knobs, and fog of war
— pictures a player can reasonably want or not want.

**Never looked at by eye:** the native board with the height ramp and the wall
tint off, which is now the only map anybody sees.

## What the Map page is down to

The Gameplay page carried **eleven** controls when the map shipped. It came down
to **two**: the map, and fog of war — and then those two became one combo and the
pane was rebuilt around them, which is the pass at the end of this section. One
more control — the pad button — moved to Input. The eight that went split three
ways, and the split is the point rather than the count.

**Not a choice** (five; "Five map controls that were not choices" above): the
style, the height ramp, the sight-blocking tint, the marker layer with its four
classes, and the save points.

**Not a choice** (three more, same argument, this pass):

- **Pause while the full-screen map is open.** On. The full map is the whole area
  over a dimmed picture with no chrome and `NoInputs` — a screen you stop at, and
  reading it takes as long as it takes. King's Field's own map was an *item*, used
  from a menu that already stopped the game; this one opens with a button in the
  middle of a corridor. Offering "…but keep the game running" is offering to put a
  full-screen surface between the player and something walking up behind them.
  `KF2_MAP_PAUSE=0`.
- **Floor.** Follows the player. Pinning a stacked half is for looking at the
  *other* one, which is a thing you do while checking the map is right, not while
  playing — and the followed answer is the measured one: the founding equality is
  that the player stands on a drawn half whose `-(height << 7)` equals their Y,
  gap 0 over three save slots, two areas and both floors. `KF2_MAP_FLOOR=lower`
  or `upper`.
- **You are here.** The dot. The argument is already written down under "You are
  here, approximately" and it is about the game, not about the player's screen: an
  arrow gives a sub-tile position and a heading to a twelfth of a degree in a maze
  whose difficulty is being lost in it. A combo asks the player to settle how much
  the port should give away. The arrow is kept in the code because what it records
  about `func_80028080`'s heading is measured. `KF2_MAP_ARROW=1`.
- **The fog's line-of-sight gate.** On, and the fog's own checkbox stays. Without
  the gate the cull cone paints an expanding wedge behind the wall beside a
  doorway — measured, 110 of 136 lit cells in area 7 sitting behind a wall mass
  the player cannot see past — into a store that never forgets. A permanent lie in
  a save file is a defect. `KF2_MAP_FOG_LOS=0`.

**A comparison rather than a feature** (the corner minimap, and its six knobs —
corner, edge padding, shape, size, range, opacity). It is off because *the picture
has never been judged by eye*, which is this port's standing reason for a default
— and a picture in that state is a poor thing to hang seven controls off, since
every one of them tunes something nobody has established is usable. So the whole
block went, the fields keep the values that shipped (a fully opaque square, 220
px, 12 tiles, top right), `KF2_MAP_MINIMAP=1` brings it back for a run, and **N
still toggles it for the session**, which is how it gets looked at. If it is
judged good the block comes back; that is what "for the time being" means here.

**Somewhere else** (the pad button). `patches/settings/MapButtonPage.cs`, under
Input, sorted between the keyboard layout and mouse look. It sat on the Map page
because it arrived with the map, which is the wrong reason: a player looking for
what a button does looks under Input, and nothing else in the port asks them to
look elsewhere for one binding. What the map *is* stays under Gameplay. The page
disables itself and says so when the map is switched off, rather than vanishing —
a binding that disappears reads as a bug.

**Retired keys.** `kf2.map.minimap` and its six, `kf2.map.pause`, `kf2.map.floor`,
`kf2.map.player`, `kf2.map.fog.sight`. None is read any more, for the reason the
last pass retired the other nine: a key still read after its control is gone
strands whoever set it. `kf2.map.on`, `kf2.map.pad.button` and `kf2.map.fog` are
what is left. Session-only controls — N, and the docked panel's Markers, Height,
Walls, Sight, Forget and Reveal — deliberately write nothing.

**Where the instruments went.** `MapPanel`'s toolbar is now the whole comparison
set: zoom, follow, grid, markers, height shading, wall tint, the fog gate, and
Forget/Reveal this area. That panel is `Shift+M`, it is session-only already, and
it is what a windowed debugger over the game is *for* — which is the same
sentence that took its palette off the player's map in the first place.

### Then the page itself was looked at

Two switches was the right *set*. Opened and looked at, the pane they made was
not, and none of the four things wrong with it was about which options exist.

**The order was an accident.** Both pages left `Order` at its default 0, so the
pane sorted on the `Title` ordinal tie-break — "Auto reload" < "Map" — which is
precisely the accident `IPatchPage.Order` was added to stop the first time, in
Video. It is also a trap for whatever comes next: a third gameplay page at the
default 0 whose title sorts between the two would have split a shared heading in
half. The map leads now (10, then 20): what the game *has* before what happens
when you fail at it, and the shorter block first.

**The map and fog of war were one question asked twice.** Two ticks cross into
four states carrying three meanings and fog-on-with-the-map-off is nobody's
answer — the same sentence, and the same fix, as "Two shading checkboxes were one
question asked twice" above. One combo: `Off` / `Whole area` / `Fill in as you
go`. `kf2.map.on` and `kf2.map.fog` are both still written and both still read,
`KF2_MAP` and `KF2_MAP_FOG` are untouched, and the map is the master on read-back
— the state only `KF2_MAP=0 KF2_MAP_FOG=1` can reach opens as `Off` rather than
earning an entry, and one click on any entry leaves the pair consistent.

**Three instruments were on a player-facing page.** *Simulate death*, the
`N death(s) seen, N reload(s)` census and the "no death yet" line. The button's
own defence — dying on purpose is the hard part of testing this — is a sentence
about testing, and this port already knows where its instruments go: the same
argument that sent Forget and Reveal to the docked `MapPanel`. Nothing is lost.
`AutoReload.Simulate` is still the shell's `kill` verb and the MCP `kf2_kill`
tool, the attract demo still kills itself unattended, and `AutoReload.Status` —
including the two failures a player could actually hit, "no save to reload" and
"slot N would not load" — still prints on stdout every time it changes.

**Three rules over four controls.** `SettingsPopup` draws a `SeparatorText`
naming the section before any extension runs, so "Gameplay", "Auto reload" and
"Map" stacked up with nothing between the first two. An empty `IPatchPage.Title`
now declines the heading — the degenerate case of the rule that pages sharing a
title share one — and `PatchSettings.Draw` keeps the `Spacing` outside the guard
so two title-less pages still get air between them. This is deliberately *not*
`DrawBare`, which is the slots' path: these pages still want their spacing and
their order, they just do not name themselves under a rule that already did.

Two smaller things went with them. The Save slot combo stretched to the full pane
width, pushing its label out to the right margin — Video's combos omit
`SetNextItemWidth` on purpose, to line up with the runtime's own, and Gameplay has
no runtime content to line up with, so both combos take `MapButtonPage`'s 260. And
the pane had both dependent-control idioms at once: the slot stayed live with auto
reload off, while fog vanished on an early `return`. The slot is now indented and
`BeginDisabled`, which is `MapButtonPage`'s rule and `FrameSmoothingPage`'s — a
control that disappears reads as a setting that was lost — and fog is inside the
combo, so there is one idiom left on the page.

The whole pane, three widgets:

```
— Gameplay ————————————————————

  Map        [ Fill in as you go ▼ ]

  [x] Reload the last save on death
        Save slot  [ Last used ▼ ]
```

**Never looked at by eye:** the pane at three widgets, whether "Whole area" and
"Fill in as you go" say what they do without the word *fog* in the label, and
whether the Input page reads right with a map binding on it.

## Auto start and the agent beacon

`patches/AutoStart.cs` (`KF2_AUTOSTART=<1..3>`, or `new` to stay in the New Game) and `patches/AgentBeacon.cs`
(`KF2_AGENT=1`) are the pair that lets an automated tester **get into the game and
know it got there**. Both are patches driven by an environment variable, not mod
features, for the `KF2_AUTOPAD` reason: a mod is off by default and silent when
disabled, so an agent that did not know to enable it would get nothing.

### The wall is the title, not the Continue menu

The plan for auto start assumed the loader could be called at GAME.EXE's start
menu. It cannot, and the reason took several runs to pin down:

- **With no input the port never leaves OPEN.EXE.** Measured: 110 s at the title,
  still `overlay open`. The "attract demo walks itself in after a minute" only
  happens on some idle path this build did not take; an agent cannot rely on it.
- **`KF2_AUTOPAD` cannot help** — its clock is gated behind the first `fdat` load
  (`Program.cs`), the very thing that has not happened at the title.
- **Writing `Controller.State` does not reach the boot menus.** It advanced
  OPEN.EXE's title but did nothing at GAME.EXE's menu. The path that reaches the
  game *wherever it is* is `PAD_dr` — a `PadReadEvent` listener, exactly what
  `patches/Mouse.cs` uses "so the buttons work in its menus". The buffer is
  active-low and its two button bytes are swapped against `Controller`'s layout,
  so a pressed `Controller` bit is injected as `e.Buttons &= ~((b>>8)|(b<<8))`.
- **The start menu (`func_8001B35C`) is a blocking poll loop**, so the per-frame
  main-loop and stage-3 hooks (`0x80040348`, `0x8002A550`) never run there — none
  of them fired while stuck at the menu, which is why a stage-3-hook load could not
  fire either. The menu's cursor handler is `func_8001EA14` (2 options: 0 = Load →
  `func_8001B4F4`, 1 = New Game), and injected Up/Down did **not** move it; only
  Cross registered, always confirming the New Game default.

### The route that works: New Game as a vehicle, then load over it

So auto start does not fight the cursor. It drives three steps, all through
`PAD_dr`:

1. Pulse **Start** through OPEN.EXE's intro/title → `overlay game`.
2. Pulse **Cross** at the start menu → confirm the New Game default → `overlay
   fdat02`, a real area with the main loop turning.
3. The stage-3 post-hook (`0x8002A550`), which now runs because an area is live,
   waits ~90 frames and calls `AutoReload.LoadSlot(slot)` — the same menu-free
   loader (`func_80023638` + the post-load arm) AutoReload uses on death — to load
   the chosen slot **over** the New Game. Nothing is saved; the load replaces the
   scratch character with the slot's own area and state.

`AutoReload.Reload` was refactored to share `LoadSlot`, so the delicate MIPS-ABI
stack window (`func_80024154`'s fifth/sixth args at `sp+0x10`/`sp+0x14`) lives in
one place.

**Verified, end to end, read off the `KF2_AGENT` beacon (no screenshot):**

```
KF2_AUTOSTART=2 →  overlay main → open → game → fdat02 → fdat05
                   [KF2] autostart: loaded slot 2 into area 1 (HP 46/86, LV 6)
                   beacon: level 1/area 0/slot 0  →  level 6/exp 417/area 1/slot 2
KF2_AUTOSTART=3 →  [KF2] autostart: loaded slot 3 into area 0 (HP 71/79, LV 5)
                   beacon: … → level 5/exp 291/slot 3   (matches the card title "2-3 LV 5")
```

### The beacon

`AgentBeacon` emits one `[KF2-AGENT]` line on each `OverlayLoadedEvent` and a JSON
snapshot about once a second from `VSyncEvent` (on the game thread, so its memory
reads need no marshalling):

```
[KF2-AGENT] {"overlay":"fdat05","inGame":true,"dead":false,"hp":46,"maxHp":86,
             "mp":…,"maxMp":…,"level":6,"exp":417,"area":1,"slot":2,
             "deathFrames":0,"pos":[x,y,z]}
```

`inGame` is `MaxHp != 0` — the same title-vs-in-game oracle as
`mods/kf2debug/GameState.IsInGame`; when it is false the stat fields are omitted.
That one field, printed once a second, is what an agent needs to tell "stuck at
the title" from "in an area" — the failure that used to burn a tester's whole
budget. The stat addresses are carried locally (patches cannot reach the mod's
`GameState.cs`, which the mod loader compiles separately); a future consolidation
could move the shared map into `patches/`.

## The command channel
`patches/AgentServer.cs` (`KF2_SHELL=1`, or `KF2_SHELL=<port>`; default port
27900) is the acting half beside the beacon's watching half: a small TCP server
on loopback speaking a line protocol — one request per line, one single-line
JSON response, always:

```
state                 the beacon's snapshot as JSON
load <slot 1..3>      load a save through AutoReload.LoadSlot
warp <area 0..7>      re-enter an area through the game's own entry routine
press <button> [ms]   hold a pad button for ms (default 150); one press at a time,
                      replaced by the next
kill                  drop HP to zero, the way a hit would
nearby [radius=8192]  live records of the world tables within radius units of the
                      player, nearest first, tagged by table (objects; buf6
                      "entities", whose reading is still Inferred -- TODO.md)
ending [boss|kill]    hand the game over to END.EXE; "boss" runs the post-final-
                      boss sequence first, "kill" replays the killing blow itself
map [on|off|toggle]   open or close the full-screen map, which pauses the world
goto <x> <y> <z> [yaw [pitch]]  put the player at a position in the current area, after
                      stage 3; yaw is the base heading, 0x1000 a turn. Sent before
                      an autostarted save has loaded, the load puts them back
```

A socket rather than stdin because stdout already carries the beacon and the
`[KF2]` log lines; a stdio channel would make every client demultiplex its
responses out of the logs. Off unless set, like every other agent switch: an
unasked listener is worse than one switch to find (the mouse-look precedent).
It is in `patches/` for the `KF2_AUTOPAD` reason — an agent that did not know to
enable a package would get nothing.

### Two marshal points, and why

Commands arrive on socket threads and must run on the game thread. Where they
run depends on whether the command re-enters the loader:

- **`state`, `press`, `kill`, `help`, `nearby`, `map` drain from a `VSyncEvent` listener** — the
  same place the beacon reads memory, so no cross-thread access and no new
  machinery. **`map` has a second reason to be there**: the full-screen map
  pauses the world, and a paused world does not run stage 3, so a `map` verb on
  the heavy queue could open the map and then never be able to close it. A vblank
  arrives whatever the world is doing.
- **`load` and `warp` drain from a post hook on main-loop stage 3**
  (`0x8002A550`). `func_80024154` waits on the CD by looping `func_80017818`,
  which calls VSync: running it inside the VSync event nests VSync inside
  itself and swaps overlays under a frame that is still drawing — the documented
  death of the debug panel's first warp button. Stage 3 is the game's own load
  path, which is where `AutoReload.Reload` and `AutoStart` already call
  `LoadSlot` from, and it only runs once an area is live — which is exactly the
  guarantee `load`/`warp` need. A heavy command issued anywhere else (at the
  title, say) times out after five seconds with that said in the error.

`kill` is safe at VSync: `AutoReload.Simulate` only snapshots the CPU, writes
HP, calls `func_8002A264` and restores — and the settings page already runs it
from inside Present-inside-VSync.

### `ending` exists because the last ten minutes of the game are otherwise untestable

Everything a session can reach it reaches by loading a save. The ending cannot be
loaded: it is reached by killing the final boss, and a defect reported there —
"the game crashes after you defeat the final boss" — has nowhere to be reproduced.
`ending` drains from the same stage 3 post as `load`/`warp`, and takes two forms:

- **`ending`** writes `GAME.EXE`'s own quit word at `0x80199574`. The main loop
  tests it immediately after stage 13: `1` writes boot-stub index 2 (`END.EXE`),
  `9` writes index 0 (`OPEN.EXE`, i.e. quit to title), and the loop then returns
  into the stub, which loads the file. That is the hand-over with no boss in it.
- **`ending boss`** runs the sequence that normally writes it. `fdat23`'s boss
  damage handler `func_8019FA2C` calls `func_8019F474` and then `func_8019F688`
  when the record's HP `u16` at `+0x1A` reaches zero, and `func_8019F688` is
  **two modal loops that present their own frames** — a camera pull-back of about
  ninety iterations, then a sixty-four step fade — ending in the quit word. Both
  have to be called: `func_8019F474` is what fills the pointer at `0x801A0598`
  that the sequence writes its camera through, and calling `func_8019F688` alone
  dies on `unmapped address: 0x0145505B`.

Getting there needs `warp 7` to survive, and it does not on its own — area 7's
centroid drops the player far enough below the floor to trip the bottomless-pit
test. `KF2_DEBUG_GODMODE=1` (the debug mod's `PreHook` on the death latch
`func_8002A264`) is what makes the warp survivable, so the rig is:

```bash
KF2_FPS=144 KF2_SHELL=1 KF2_AUTOSTART=2 KF2_AUTORELOAD=0 KF2_DEBUG_GODMODE=1 …
# then: warp 7   (wait)   ending boss
```

- **`ending kill`** is the form that **does** reproduce the crash, and it exists
  because `ending boss` never could. `ending boss` calls the two loops directly,
  and the crash is not in them: it is in the hit resolution *underneath* them,
  which that route never puts on the stack. `ending kill` replays the two calls
  that matter, in the game's own order — `fdat23`'s damage hook
  `func_8019FA2C(entity 0, 1)`, and then the death reaction
  `func_8003A490(entity 0, 3)` that `func_8003A9CC` makes next — having first set
  the preconditions the fight leaves behind (`u8[0x801B30A2] = 0`, the ending not
  yet run; `u8[0x801B30A6] = 1`, the phase-two transition already done, so the
  hook takes its ending branch rather than the HP top-up that survives the first
  kill; `u16[0x8016C55E] = 1`, the boss on its last point). It prints entity 0's
  type byte after the hook and the fifteen descriptor pointers, so the run says
  what it found even though the reply times out — both `boss` and `kill` run for
  far longer than the channel's five-second reply deadline, so the JSON never
  reaches the client and stdout is the record.

**It caught the bug it was built for.** Measured at `KF2_FPS=165` in area 7:
`entity 0 type is now 255, descriptor 0x80179DAC, pointers: 0FFF0000 FF380320
00000000 …` — the first pointer being `0x0FFF0000`, the exact value the reported
crash died on. With `KF2_HITGUARD=0` the death reaction faults there; with the
guard on it answers 0 and the ending hands over to `END.EXE`. See "The crash on
the final boss's last hit" in `docs/TODO.md`.

Getting there is still the `warp 7` + `KF2_DEBUG_GODMODE=1` rig above; without
godmode the warp kills the player on the way in and the session reaches
`END.EXE` by the death route instead, with `fdat23` already unloaded.

### One implementation of the warp

The warp core moved out of the debug mod into `patches/AreaWarp.cs`
(`Kf2.AreaWarp.TryRun`) so the server can reach it with no package enabled;
the mod's panel delegates to it, so there is still exactly one transcription of
the game's area-entry sequence. The synthetic press keeps its own inject state
rather than sharing `AutoStart._inject`: that field belongs to the boot driver,
and the two must never fight over it. The press itself rides `PadReadEvent` with
AutoStart's active-low byte-swapped math, which is what makes it work in the
boot menus and everywhere else the game reads the pad.

## The MCP layer

`mcp/` (`KingsField2Mcp.csproj`) is a standalone stdio MCP server that wraps
exactly the six verbs above as typed tools, so an MCP host — Claude Desktop, an
inspector, omp — can drive the same channel without knowing the line protocol.
It is a separate program with no NuGet dependencies (hand-rolled JSON-RPC over
stdin/stdout; stdout carries protocol messages only), it talks TCP loopback to
the shell, and the game itself needs no patch, no recompile and no config entry.

The endpoint defaults to `127.0.0.1:27900`; `KF2_MCP_ENDPOINT=<host:port>`
points a server instance elsewhere (a second game session on
`KF2_SHELL=<port>`, say) without touching the game's own switch.

```json
{ "mcpServers": { "kf2": { "command": "dotnet",
    "args": ["run", "--project", "<repo>/mcp/KingsField2Mcp.csproj", "-c", "Release", "--no-build"] } } }
```

The tools are `kf2_state`, `kf2_nearby`, `kf2_load_save`, `kf2_warp`,
`kf2_press_button` and `kf2_kill`, one per verb, with the button names and
ranges quoted from `AgentServer`. The shell stays the validator: replies come
back verbatim, an `ok:false` surfaces as a tool error, and a game that is not
running costs every tool one connection error rather than a dead host session.


## The polygon assembler in C#

`patches/PolyAssembler.cs` replaces `func_80030540` — the routine that turns one
projected mesh into `POLY_GT3`/`POLY_GT4` packets and links them into the ordering
table, for the map tiles (`func_80031950`) and the models (`func_80032588`) alike —
with a hand-written C# body behind a `Replace` hook. It is on by default;
`KF2_POLYASM=0` is the recompiled routine and `KF2_POLYASM=verify` runs both.

**Video ▸ Frame pacing ▸ Fast geometry** is the one control, saved as
`kf2.fastgeometry.on` and on by default: it switches the assembler and every
routine below it together with the GTE fast path, mid-session. The hooks are
installed whatever `KF2_POLYASM` says so it can be turned on after booting with
`=0`, and `KF2_POLYASM` or `KF2_GTE_FAST` set on the command line wins over the
saved value for the run. It replaced a temporary Optimizations page that switched
each routine on its own beside live call rates; the per-routine switches are the
`KF2_POLYASM_*` and `KF2_GTE_*` variables now, and the frame profiler (Shift+P) is
where the comparison is read.

**What it keeps, and why.** Every `ReadU32` and `WriteU32` the recompiled routine
makes on a vertex word, a packet or the ordering table is made in the same order,
through `PSMemory`, because that is what `GteVertexMap` follows into the packet; a
store straight into the RAM array would leave perspective correction, sub-pixel,
the Z-buffer and AO with nothing bound. The libgte leaves it calls are inlined as
the same `Gte` calls the recompiler emits for them (`NormalClip`, `NormalColorCol`,
`DpqColor`, `AddPrim`); the vertex transform `func_8002E7CC`, both clippers
(`Clip4FTP`, `Clip3FTP`) and the clipped-polygon emitter `func_800302E8` are
called as recompiled code with the arguments and stack words the original sets, so
any hook on them still fires. Two things are dropped: the saved-register block and
the scratch words on its own stack (the lighting result is stored at `sp+0x50` and
read straight back), which nothing reads after the call returns. PGXP's CPU
tracking follows values through registers that locals do not have, so with
`Pgxp.CpuTracking` on the hook calls the recompiled routine.

**Verified byte for byte.** `KF2_POLYASM=verify` snapshots RAM and the CPU context,
runs the recompiled routine, snapshots its result, restores, runs the C# one, and
compares all of RAM except the `0x2000` bytes below the entry `SP` — the two
frames' garbage — plus `S0`-`S7`, `FP`, `SP` and `RA`. The recompiled result is put
back afterwards, so a mismatch can never reach the picture. Over the command-channel
tour of all eight areas, standing and turning (see "Where stage 13's time goes" in
[DEVELOPMENT.md](DEVELOPMENT.md)): **879,556 calls, 0 RAM and 0 register
mismatches**. A second run over areas 2, 5 and 7 counted the rare paths: 543,964
calls, 0 mismatches, 2,835,903 quads and 2,139,238 triangles sent to the clippers,
869,560 clipped pieces emitted. The buffer-exhaustion return was never taken
(`PrimBuffer.cs` has never seen it either), so that one path is checked by reading
only. What the verify run does not compare is `GteVertexMap`'s own state, which both
runs feed, so that was measured separately: `KF2_PERSPECTIVE_PROBE=1`, area 1 from
slot 2 standing, `KF2_FPS=1000`, reads 63.0% hit with the patch off and on, and
255.0 vertices projected, 402 copied and 638 looked up a frame both ways.

**What it bought.** Areas 2, 5 and 7, `KF2_FPS=1000`, the same tour with each build,
`func_80030540` inclusive per frame:

| | recompiled | C# | |
|---|---|---|---|
| area 2 still | 0.947 ms | 0.813 ms | -14% |
| area 2 turning | 0.735 | 0.616 | -16% |
| area 5 still | 0.547 | 0.487 | -11% |
| area 5 turning | 0.523 | 0.462 | -12% |
| area 7 still | 0.671 | 0.603 | -10% |
| area 7 turning | 0.658 | 0.598 | -9% |

0.06-0.13 ms a frame, which the frame total cannot resolve: `st13 incl` and fps
moved by more than that in both directions between the two runs, with sections this
patch does not touch (`func_8002F214`, `func_80032588`) swinging 10-50%. The picture
is identical by construction and has not been looked at.

**Why so little: the routine was mostly its clippers.** The same tour with the four
recompiled callees timed, C# body on, self ms a frame:

| | area 7 still | area 7 turning | area 2 still | area 2 turning |
|---|---|---|---|---|
| the C# body itself | 0.119 | 0.106 | 0.155 | 0.120 |
| `func_8002E7CC` vertex transform (one a call) | 0.076 | 0.070 | 0.071 | 0.059 |
| `Clip4FTP` (calls a frame) | 0.421 (173) | 0.397 (173) | 0.019 (10) | 0.020 (9) |
| `Clip3FTP` (calls a frame) | 0.013 (8) | 0.024 (10) | 0.499 (313) | 0.405 (246) |
| `func_800302E8` clipped emitter (calls) | 0.041 (55) | 0.034 (46) | 0.045 (62) | 0.035 (47) |

Two thirds of the routine is the view-space clipper (see "The second cull" in
[WIDESCREEN.md](WIDESCREEN.md)), and **about three in four polygons sent to it come
back with fewer than three vertices** — 181 clipper calls against 55 emitted in
area 7, 323 against 62 in area 2. A face goes there when any vertex's cached `otz`
word is `0xFFFF` or an edge is past the GPU's limits, so this is largely geometry
around and behind the camera being clipped to nothing, one Sutherland–Hodgman pass
and one `RotTransPers` per surviving vertex at a time. That makes the clipper the
next target, and the cheap version is a trivial reject in front of it: a polygon
whose vertices are all outside the same one of the six planes clips to no vertices,
which is exactly the `< 3` the assembler already discards, so the packets and the
ordering table come out the same. It is not byte-identical everywhere: the clipper
also leaves its vertex records at `0x80192A18` and its last results in the GTE, so a
verify for it has to exclude that scratch and argue the GTE is written again before
it is next read.

### Rejecting what the clipper clips to nothing

`ClipFT` (`0x8005CE98`) runs six Sutherland–Hodgman passes over a list of 0x2C-byte
records, in a fixed order: far (`Z < *0x8012E99C`), near (`Z >= *0x8017E07C`),
`Y >= -ybound`, `Y <= ybound`, `X >= -xbound`, `X <= xbound`, with the bounds
`(Z · *0x800FC98C) >> 12` and `(Z · *0x800FC97C) >> 12` computed per record by
`Clip4FT`/`Clip3FT`. A pass emits an intersection only where consecutive vertices
disagree, so **a pass whose vertices are all inside hands the same records to the
next one untouched, and a pass whose vertices are all outside emits nothing** — and
every later pass then sees a count of zero. The first pass that is not all-inside
therefore sees the original vertices. If they are all outside it, the polygon clips
to nothing, which is the `< 3` the assembler already throws away; if they are mixed,
the real clipper runs.

`PolyAssembler.Reject` decides that before calling `Clip4FTP`/`Clip3FTP`: `RotTrans`
on each vertex through the same GTE call, in the clipper's own record order (`a0,
a1, a3, a2` for a quad), the two bounds, and the six predicates with the game's
signed compares. The GTE is left exactly where the clipper would have left it, and
when the polygon is not rejected the clipper repeats the same transforms, so the
GTE state after it is unchanged either way.

**A rejection still makes the clipper's writes.** The record copies (the same
`lwl`/`lwr` pairs), the UVs, the transformed position and both bounds per record,
the record list at `0x80192A18`, and the output list of every all-inside pass before
the rejecting one — with the same reads between them and the same
`Interrupts.Poll` count. That keeps RAM byte-identical, so `KF2_POLYASM=verify` still
demands zero mismatches with the rejection inside it, and keeps `GteVertexMap`'s view
of those reads and stores the same. What it does not do is the calls, the frames and
the register traffic. `KF2_POLYASM_REJECT=0` is the comparison.

**Verified:** areas 1, 2, 5 and 7 standing and turning, 638,244 calls, **0 RAM and 0
register mismatches**. Of 5,475,716 polygons too big for the GPU, 2,752,997 (50.3%)
were rejected before the clipper, 2,722,719 still went to it, and 807,250 clipped
pieces were emitted — so the reject catches 59% of what used to clip to nothing, and
the rest is genuinely mixed.

**What it bought: 2-12% of the routine, because the replayed writes are most of a
clipper call.** `KF2_FPS=1000`, the same tour with `KF2_POLYASM_REJECT=0` and without,
ms a frame. Area 7 standing is left out: the two runs did not draw the same scene
(27 against 22 assembler calls a frame, 108 against 55 clipped pieces).

| | area 7 turning | area 2 still | area 2 turning |
|---|---|---|---|
| `func_80030540` inclusive | 0.630 → 0.617 | 0.801 → 0.704 | 0.660 → 0.585 |
| clipper calls a frame | 182 → 118 | 321 → 119 | 256 → 105 |
| clipper time | 0.417 → 0.339 | 0.523 → 0.285 | 0.437 → 0.253 |
| the C# body (now including the rejection) | 0.106 → 0.172 | 0.157 → 0.297 | 0.122 → 0.232 |
| clipped pieces emitted (unchanged) | 45.9 / 43.3 | 62.0 / 62.0 | 46.9 / 46.7 |

A clipper call costs about 1.6 µs; a rejection in C# about 0.7 µs, nearly all of it
the record copies (each `lwl`/`lwr` is a read and a read-modify-write), the stores
and the replayed reads that keep RAM identical. **The lean version is the obvious
next step and a trade**: skipping the clipper's scratch — the records at
`*0x8006E7B0` and the lists at `0x80192A18` — would cut a rejection to the four
transforms and the compares, but RAM would then differ from the recompiled routine
there, so the verify mode would have to exclude those ranges, and it would need a
check first that nothing but the clipper reads them.

Done since: that check came back clean and the lean version is now the default. See
"The lean rejection" below; `KF2_POLYASM_REJECT=replay` is the version above.

### The unclipped assembler and its transform

`func_8002FECC` is `func_80030540` without a clipper. `func_80031950` sends it
every map tile whose visibility cell lacks bit `0x80`, which is the flood's lit set
and so the far bulk of the map (see "What the map walk's time is" in
[DEVELOPMENT.md](DEVELOPMENT.md)). It calls `func_8002E650` to fill the vertex
cache, then for each face: `NormalClip`, allocate, the same `POLY_GT4`/`POLY_GT3`
packet `func_80030540` writes, and `AddPrim`. Both are in `PolyAssembler.cs` beside
the assembler, as two more replace hooks under the same `KF2_POLYASM` switch, with
`KF2_POLYASM_UNCLIPPED=0` and `KF2_POLYASM_TRANSFORM=0` to compare each on its own.

**It differs from `func_80030540` in only three places**, so the packet fill is now
shared (`FillQuad`, `FillTriangle`):

- no size test and no clipper, so a polygon past the GPU's limits is sent as it is;
- `NormalClip` loads the third vertex before the second (`Facing`), a read order
  kept because a load of a bound vertex word republishes it to the vertex map;
- the ordering-table slot is the average otz plus a fixed `0xF0`, and a slot of
  `0x2000` or more is **dropped**, not clamped, after the packet has already been
  allocated (`Place`).

`func_8002E650` runs `RotTransPers` over the mesh into the cache at `0x8018EB94`:
the screen word, otz as `SZ3 >> 2`, and a fog weight from `IR0` on one of three
curves picked by the word at `0x80192EA8` — zero at `>= 32000`, `max(IR0 - 800, 0)
* 2` when bit `0x8000` is set, else `IR0` below 2800 and `3 * IR0 - 0x15E0` above.
`RotTransPers` stores `IR0` and the flag on the stack, and the routine reads `IR0`
back only for the weight, so both stay in locals. That leaves out two stores a vertex
from the vertex map's tick, which is what ages its pending values. The probe below
shows it changes nothing. `func_80032588`, `func_80032400` and `func_800346CC` call
the transform too, so the models and the arm get it as well.

The C# assembler calls the transform through `KingsField2.func_8002E650`, so it
goes through the detour: the profiler still sees it, and the verify mode checks each
on its own. Each routine has its own verify buffers, because the transform is
verified again inside both runs of the assembler.

**Verified:** areas 1, 2, 5 and 7 standing and turning, `KF2_DEBUG_GODMODE=1` so
area 7 does not kill the player: **150,074 calls of `func_8002FECC` and 327,015 of
`func_8002E650`, 0 RAM and 0 register mismatches** (and 109,858 of
`func_80030540` after the shared packet fill, 0 and 0). The buffer-exhaustion return
was not taken. `KF2_PERSPECTIVE_PROBE=1` reads 95.1%, 96.4% and 92.7% hit at the
same three points of the tour with both off and on, with the same vertices per frame.

**What it bought.** The tour, `KF2_FPS=1000`, both off and then both on, ms a frame
(calls of `func_8002FECC` a frame in brackets):

| | `func_8002FECC` incl | `func_8002E650` | map walk (`func_80031C94` incl) | work | fps |
|---|---|---|---|---|---|
| area 2 still (67.7) | 0.555 → 0.380 | 0.157 → 0.090 | 1.428 → 1.218 | 2.791 → 2.524 | 344 → 378 |
| area 2 turning (76) | 0.489 → 0.341 | 0.208 → 0.125 | 1.174 → 1.009 | 2.915 → 2.695 | 323 → 346 |
| area 5 still (33.0) | 0.270 → 0.184 | 0.068 → 0.039 | 0.779 → 0.675 | 1.604 → 1.457 | 582 → 635 |
| area 5 turning (58) | 0.302 → 0.202 | 0.138 → 0.081 | 0.922 → 0.802 | 2.198 → 2.041 | 430 → 457 |
| area 7 still (89 / 86) | 0.320 → 0.206 | 0.231 → 0.133 | 1.396 → 1.239 | 3.967 → 3.692 | 245 → 262 |
| area 7 turning (67) | 0.265 → 0.179 | 0.166 → 0.101 | 1.190 → 1.078 | 3.091 → 2.925 | 273 → 288 |

**30-36% off the assembler including its transform, and 40-43% off the transform
alone.** That is more than `func_80030540` gained, because nothing here is a
clipper. It moved the map walk by 0.10-0.21 ms and the frame's work by about as
much. Area 1 barely uses it (0 to 2 calls a frame). Area 7 standing drew slightly
different scenes in the two runs (104 against 108 tile submits). The picture is
identical by construction and has not been looked at.

### The lit model assembler

`func_8002F214` is 75-80% of the model submitter `func_80032588` wherever that is
heavy (see "What the model submitter's time is" in [DEVELOPMENT.md](DEVELOPMENT.md)),
and `func_8002EAEC` is the same routine forced semi-transparent. Both are in
`patches/PolyAssemblerLit.cs`, a second half of the `PolyAssembler` class, as two
more replace hooks under `KF2_POLYASM`, with `KF2_POLYASM_LIT=0` to compare.

**It is `func_8002FECC`'s loop with lighting.** The same header, vertex cache,
`Facing` load order, allocator and `Place`, so those are shared. What differs:

- four TMD types, each its own packet: `0x24` `POLY_FT3`, `0x2C` `POLY_FT4`,
  `0x34` `POLY_GT3`, `0x3C` `POLY_GT4`, with the mode byte as the code;
- flat faces call `NormalColorDpq` at the mean fog weight (a quad sums it
  `p0, p1, p3, p2`), gouraud faces `NormalColorDpq3` at the first vertex's weight
  and a quad a second `NormalColorDpq` for its fourth normal. Both are inlined
  (`Shade`, `Shade3`) and their stack arguments stay in locals;
- a quad reads its fourth vertex index between the allocator's read of the buffer
  pointer and its store, so it has its own `Allocate` overload;
- the slot is the mean otz plus `a1`, and a mean `<= 0` is dropped before
  `Place`'s range test;
- `func_8002EAEC` writes `0x26`/`0x2E`/`0x36`/`0x3E` as the code and puts
  `a2 << 5` into bits 5-6 of the word it copies from the face's `+0x6`.

**Verified:** areas 1, 2, 3, 5 and 7 standing and turning, `KF2_POLYASM=verify`,
`KF2_DEBUG_GODMODE=1`: **164,677 calls of `func_8002F214` and 13,809 of
`func_8002EAEC`, 0 RAM and 0 register mismatches**, with every other routine in
the class still at 0 and 0. Unlike the other assemblers, **the buffer-exhaustion
return was taken** here, 1,195 times between the two, all in areas 2 and 7, and
the recompiled routine took it the same way. Where the buffer runs out has not
been looked into. `KF2_PERSPECTIVE_PROBE=1`, `warp 7` standing at 144 fps, five
runs: an on and an off run that drew the same scene (the same creatures at the
same distances by `nearby`) read 96.2-96.3% hit and 43-49k saturated a second
each, second for second. The other three read 96.3-96.4% (off, 9-12k saturated),
96.3% (on) and 95.6% (on, the creatures elsewhere). Saturation is counted in
`Gte.Rtp`, which this code does not reach, so it follows the scene. 144.0 fps drawn
at 20.0 ticks/s, `[present] wide 288`.

**What it bought.** The scene at a warp point is not repeatable (creatures walk
into and out of view), so this was measured in one session per area: a temporary
build switched the hook between the two every 2 s on the profiler's clock, and
frames were split by which half they started and ended in. `KF2_FPS=1000`,
`func_8002F214` inclusive, ms a frame (µs a call):

| | recompiled | C# | a call |
|---|---|---|---|
| area 2 still | 0.061 (4.1) | 0.045 (3.0) | -27% |
| area 3 still | 0.086 (4.8) | 0.059 (3.3) | -31% |
| area 5 still | 0.046 (3.1) | 0.034 (2.3) | -26% |
| area 6 still | 0.457 (26.6) | 0.360 (20.8) | -22% |
| area 7 still | 0.151 (8.8) | 0.115 (6.7) | -24% |
| area 7 turning | 0.591 (31.5) | 0.409 (22.6) | -28% |

**22-31% a call, less than `func_8002FECC`'s 30-36%**, because most of a lit face's
cost is the GTE's lighting, which runs the same in both. Turning halves do not
see the same view, so only area 7's, whose call counts matched, is listed. In
area 7 turning, `func_80032588` came out 0.744 against 0.549 ms a frame. The
picture is identical by construction and has not been looked at.

### The lean rejection

**Nothing reads the clipper's scratch before the clipper writes it again**, so a
rejection no longer writes it. `ClipFT` fills each new record and each output-list
slot before it reads them, the records it reads past the ones `Clip4FT`/`Clip3FT`
just filled are the ones `ZClipFT` just made, and the only other reader of the lists
at `0x80192A18` is the clipped-polygon emitter, which runs only after a real clip.
A rejection now does the four transforms, the six plane tests (as one 6-bit
inside-mask per vertex: the AND of the masks says which passes are all-inside, the
lowest bit it lacks is the first pass that is not, and the OR says whether any vertex
is inside that one) and the same `Interrupts.Poll` count, and nothing else.
`KF2_POLYASM_REJECT=replay` puts the scratch
writes back. `KF2_POLYASM=verify` copies the recompiled routine's records (four at
most) and both lists into its own result before comparing, since those are exactly
the bytes it no longer writes.

### The near transform

`func_8002E7CC` is the assembler's own vertex transform, and is `func_8002E650`'s
shape with two differences: otz is written as `0xFFFF` unless `RotTransPers`'s flag
is **exactly** `0x1000` (the depth cue saturated and nothing else), which is what
sends a face near the camera to the clipper, and there are only two fog curves (zero
at `FogMode >= 32000`, else `IR0` below 2800 and `3 * IR0 - 0x15E0` above). It is a
fourth replace hook under `KF2_POLYASM_TRANSFORM`, which now covers both transforms.
Like `func_8002E650` it keeps the depth cue and the flag in locals rather than on
its stack, which leaves those stores out of the vertex map's store count.

### The clipper in C#

`Clip4FTP` and `Clip3FTP` are two more replace hooks, in `patches/PolyAssemblerClip.cs`
(`KF2_POLYASM_CLIPPER=0` to compare): the record copies, `RotTrans` and both bounds
(`Clip4FT`/`Clip3FT`), the six passes (`ClipFT`), the crossing records
(`ZClipFT`, with `LoadAverageShort12` and `LoadAverageByte` inlined as the same
`Gpf`/`Gpl` GTE calls), and `RotTransPers` on each survivor. Every load and store of
RAM is the recompiled routine's, in its order, including the re-reads of
`*0x8006E7B0` it makes before every copy; what is gone is each routine's register
saves and the denominators and flags it keeps on its own stack. The two side-plane
kinds find the crossing on different lines -- `(s * coord >> 12) + z` for the
`-bound` planes and `z - (s * coord >> 12)` for the `+bound` ones, with `s` read from
`0x800FC994` for Y and `0x800FC984` for X, not the bound scales -- and the far pass
tests `Z < *0x8012E99C` while the near pass tests `Z >= *0x8017E07C`.

### What a call no longer does per face

Three things the assemblers did on every face that the vertex map never sees:

- **16- and 8-bit loads and stores go straight into RAM.** `GteVertexMap` follows
  whole words only, so while `PSMemory` would do nothing else with a narrow access
  -- nothing frozen, no RAM logger, no overlay waiting on its header, which the
  runtime now answers as `PSMemory.DirectRam` -- a face's indices, UVs, otz and fog
  words and its packet's tpage, UV and code bytes skip the checked path. An address
  below 2 MB is inside RAM whatever its size, so that is the only per-access test.
- **Three words are read once a call instead of once a face**: the light colour
  (`0x8006E604`), the ordering-table base (`0x8018E0A8`) and the primitive
  descriptor (`0x8017E0A4`), plus the allocator's re-read of the pointer it has just
  stored. A load of a word the vertex map has nothing bound to has no effect at all,
  so each is skipped only while its presence bit is clear.
- **The blend twin is its own code**: `func_8002EAEC`'s differences are a type
  argument, so neither routine tests for them per face.

Only an interrupt handler could change the first two answers mid-call, and handlers
only run from `Interrupts.PollSlow`, so the runtime counts those (`SlowPolls`) and a
call takes both answers again when the count moves, and after any recompiled callee.

### The GTE fast path

`patches/recompone/0047`. Every GTE op this game calls in these routines is the same
form each time -- `NormalColorDpq`, `NormalColorDpq3` and `NormalColorCol` at `sf=12,
lm=1`; `DpqColor`, `RotTrans` and `RotTransPers` at `sf=12, lm=0` -- and the
runtime's general path takes `sf` and `lm` as arguments through `MatVec`, `SetMac`
and `SatIR`, so every shift is variable, `SetMac` branches on which MAC it is, and
every saturation stores the flag register through `Flag()`. `Gte.NcdsOp`, `NcdtOp`,
`NccsOp`, `Dpcs`, `MvmvaOp` (the `RotTrans` form) and `Rtps` now take a fast path for
that form, with the constants folded and the flags gathered in a local, for **every**
caller including the recompiled game's own. `Rtp`'s bookkeeping after the divide
(PGXP, the vertex map's depth and fraction, `GteDepth`) is one method both paths
call. `KF2_GTE_FAST=0` is the general path.

A lighting op is two matrix products and then a depth cue or a colour modulation, and
the products depend only on the normal and on `LLM`, `BK` and `LCM`, so they are also
**remembered per normal**: a 1,024-slot table keyed on the normal's three components
and a generation that any write to control registers 8-20 bumps. A hit supplies
`IR1`-`IR3` and the flag bits the products raised; everything after them runs as
before, so the registers come out the same. `KF2_GTE_LIGHTCACHE=0` turns it off.

**Verified two ways.** A differential test against the runtime DLL (not in the repo)
runs each op from 300,000 random register states, both paths, including repeated
normals, matrix writes between ops and saturating values, and compares every register
and the projected-vertex slots after each op: **3,600,000 ops, 0 mismatches**. In the
game, `KF2_POLYASM=verify` now also snapshots the GTE, restores it before the C# run
and compares it after (a GTE mismatch is a third count beside RAM and registers). A
single-op benchmark, ns an op, general / fast / fast with every normal cached:

| op | general | fast | cached |
|---|---|---|---|
| `NcdsOp` | 60.8 | 17.0 | 10.2 |
| `NcdtOp` | 178.9 | 65.6 | 30.9 |
| `NccsOp` | 51.7 | 15.6 | 8.7 |
| `Dpcs` | 23.6 | 7.6 | |
| `MvmvaOp` (RotTrans) | 21.6 | 6.4 | |
| `Rtps` | 27.6 | 17.7 | |

### All of the above, verified and measured

**Verified**, the tour over areas 1, 2, 3, 5, 6 and 7 standing and turning with
`KF2_POLYASM=verify` and `KF2_DEBUG_GODMODE=1`, RAM, registers and the GTE, twice:
before the clipper port, 112,838 calls of `func_80030540`, 225,672 of
`func_8002E7CC`, 392,652 of `func_8002E650`, 179,270 of `func_8002FECC`, 167,685 of
`func_8002F214` and 10,693 of `func_8002EAEC`, with 1,270,766 polygons rejected lean
and 513 buffer exhaustions in the lit pair; after it, 305,615 calls of `Clip4FTP` and
352,043 of `Clip3FTP` (70,947 clipped pieces emitted) with every other routine
verified again. **0 RAM, 0 register and 0 GTE mismatches** in both.
`KF2_PERSPECTIVE_PROBE=1`, area 1 standing, everything off (`KF2_POLYASM_CLIPPER=0
KF2_POLYASM_TRANSFORM=0 KF2_POLYASM_REJECT=replay KF2_GTE_FAST=0
KF2_GTE_LIGHTCACHE=0`) and everything on: 63.0% hit both ways, at 204-231k vertices
projected a second. Areas 2 and 7 stayed within 90.5-96.6% both ways, but the scenes
differed (area 7 saturated 27-48k vertices a second in one run and 36-225k in the
other), so those are inside the band rather than a comparison.

**Measured**, the same tour at `KF2_FPS=1000` with the ten routines timed, once on
the build before any of this and once with all of it, ms a frame (µs a call). *Work*
is the frame less `buffer swap + driver` and the throttle: the swap alone read
0.87-2.2 ms a frame in the first run against 0.12-0.42 in the second, so frame totals
and fps are not comparable between the two runs and neither is quoted.

| | area 1 turning | area 2 still | area 5 turning | area 6 turning | area 7 still |
|---|---|---|---|---|---|
| `func_80030540` incl | 0.386 → 0.205 | 0.511 → 0.306 | 0.359 → 0.183 | 0.541 → 0.280 | 0.620 → 0.433 |
| clipper (`Clip4FTP` + `Clip3FTP`) | 0.142 → 0.094 | 0.158 → 0.124 | 0.133 → 0.090 | 0.217 → 0.146 | 0.331 → 0.254 |
| `func_8002E7CC` | 0.050 → 0.027 | 0.061 → 0.034 | 0.047 → 0.026 | 0.058 → 0.032 | 0.067 → 0.041 |
| `func_8002F214` | 0.078 → 0.052 (5.3 → 3.5) | | 0.288 → 0.157 (15.5 → 9.2) | 0.287 → 0.180 (15.8 → 10.3) | 0.107 → 0.077 (6.2 → 4.7) |
| `func_8002FECC` | | 0.210 → 0.160 | 0.228 → 0.176 | 0.124 → 0.093 | 0.202 → 0.170 |
| map walk (`func_80031C94` incl) | 0.380 → 0.258 | 0.792 → 0.537 | 0.711 → 0.473 | 0.913 → 0.609 | 1.110 → 0.888 |
| work | 1.218 → 0.909 | 1.651 → 1.364 | 1.839 → 1.332 | 2.017 → 1.511 | 2.111 → 1.790 |

A clipper call went from 2.1 to 1.5 µs and a `func_80030540` call is 30-52% cheaper
over every phase of the tour. Area 2's lit calls and areas 3 and 5 standing drew
different model counts in the two runs (`func_80032588` 4 against 7 calls a frame in
area 5), so they are left out.

**The GTE fast path's own share**, measured in one session with a temporary build
that switched `Gte.FastLighting` and `Gte.LightCache` together every 2 s on the
profiler's clock (the two paths give the same registers, so switching mid-frame is
safe), frames split by the half they started and ended in. Standing still, where the
call counts matched:

| | area 2 still | area 5 still | area 6 still | area 7 still |
|---|---|---|---|---|
| `func_80030540` incl | -13% | -13% | -11% | -12% |
| `func_8002F214` | -41% | -22% | -18% | -17% |
| `func_8002FECC` | -19% | -20% | -18% | -16% |
| transforms | -16%, -18% | -17%, -15% | -16%, -15% | -16%, -10% |
| clipper | -6% | -5% | -6% | -5% |
| work | 1.963 → 1.715 ms | 0.910 → 0.850 | 2.801 → 2.570 | 1.622 → 1.555 |

The picture is identical by construction -- every store the game's routines make is
made, with the same value -- and has not been looked at.

### The map tile walk in C#

**Mechanism measured against the recompiled routine on every call; the picture has
not been looked at, and does not need to be — every store the game's routines make
is made, with the same value.**

`patches/TileWalk.cs` takes the three routines between `CullGrid` and
`PolyAssembler`: `func_80031C94` (the 24x24 cell sweep), `func_80031B1C` (one cell's
two stacked halves) and `func_80031950` (one half, set up and handed to an
assembler). `KF2_TILEWALK=0` is the comparison and `KF2_TILEWALK=verify` the proof;
`KF2_TILEWALK_CELL=0` and `KF2_TILEWALK_TILE=0` take one routine back on its own.

**Why this one, when its own body is 0.041-0.124 ms a frame.** It is not a
performance change and must not be argued as one — the numbers below say so
plainly. It is the routine at which the port learns *which tile, at what world
position, is drawn by which assembler*, and it was the last recompiled step in a
path that is C# at both ends: `CullGrid` already replaces `func_8002D3A8` and writes
the 24x24 grid back to `0x80192EAC`, which is the array this walk reads, and
`PolyAssembler` already owns two of the three assemblers it dispatches to. That is
the same move `0050` made when the assemblers began recording each packet's depth:
what closed "unbridgeable" was not a better rasterizer but **knowing which routine
asked**, and no consumer of GP0 can recover that. Everything a screen-space
technique wants next — shadow maps, probe lighting, anything that draws the world
twice — needs a scene, and this walk is where the world's geometry is enumerated.

**What the three routines are.** The grid is 24x24 bytes at `0x80192EAC`, its origin
tile the two words at `0x80192EA0`/`0x80192EA4`. A row whose world Z is off the
80x80 map is skipped whole and so is a column whose X is; a cell with a non-zero
byte goes to `func_80031B1C`. The map is 80x80 tiles of 10 bytes at `0x801C8484`:
`+0`/`+1` the lower half's model and height, `+5`/`+6` the upper's, and a half is
drawn only while its model byte is **below 240** and its bit in the cell byte
(bit 0 lower, bit 1 upper) is set. The position is built in the caller's own frame
as three shorts — `(tile << 11) - camera + 0x400` on X and Z, `(-height << 7) -
camera` on Y — and **that pointer is what `func_80031950` receives**, which is why
the C# keeps the original's stack arithmetic exactly: `EvenFog` and `PacketMatch`
both read it off `a1`. `func_80031950` then loads the view matrix, rotates by the
record's low two bits, takes the light and colour matrices from the area's 104-byte
record at `0x801930F0 + (byte4 & 0x3F) * 104`, and picks the assembler on the cell
byte: no `0x80` is `func_8002FECC`, `0x80` is `func_80030540`, and `0xC0` on a mesh
of fewer than 16 faces goes through `func_80030C94`'s subdivider first.

**Verified.** `KF2_TILEWALK=verify` runs both versions on every call from the same
registers, RAM and GTE state, compares all three, and lets the recompiled result
stand so a mismatch cannot reach the picture. The stack window is 0x4000 rather than
PolyAssembler's 0x2000, because `func_80031950` alone takes 0x1050 of frame for the
subdivider's scratch. Measured in `fdat02`, each routine on its own and then all
three together:

| routine | calls verified | RAM | registers | GTE |
|---|---|---|---|---|
| `func_80031C94` | 6,900 | 0 | 0 | 0 |
| `func_80031B1C` | 178,673 | 0 | 0 | 0 |
| `func_80031950` | 68,041 | 0 | 0 | 0 |

**What it cost, which is nearly nothing, and that was the prediction.** The frame
profiler, `fdat02`, the median of 2,500 steady-state frames, with `KF2_TILEWALK=0`
and then on. The call counts are identical either way (1 walk, 192 cells, 145
halves a frame), which is what says the two runs drew the same scene:

| | recompiled | C# | |
|---|---|---|---|
| `func_80031C94` inclusive | 2.734 ms | 2.721 ms | -0.5% |
| the walk's own body | 0.0258 | 0.0244 | -5.4% |
| the cell routine's own body | 0.0550 | 0.0514 | -6.5% |
| the tile routine's own body | 0.6147 | 0.5993 | -2.5% |

Each routine is a few percent cheaper and the walk as a whole is 0.013 ms a frame
cheaper, which is inside the noise of anything but a profiler. **The tile routine
barely moved because almost none of it is now C#**: its body is ten recompiled
libgte calls per half — `SetRotMatrix`, `SetTransMatrix`, `RotTrans`,
`func_80014B88`, the two matrices again, `SetLightMatrix`, `SetColorMatrix`,
`func_8002DDDC`, `SetBackColor` — about 1,450 recompiled calls a frame that this
change did not touch. Inlining them as direct `Gte` calls, the way `PolyAssembler`
inlines `NormalClip` and `DpqColor`, is the follow-up and is where the time is.

**Two things the port kept on purpose.** The two `Interrupts.Poll` calls at the
recompiled loop heads are still there — 576 iterations is long enough to starve an
IRQ without them — and the guest-stack prologue and epilogue are the original's
byte for byte, so the pointer `func_80031950` hands its hooks is unchanged.

`KF2_TILEWALK_PROBE=1` is the semantic tap made visible: `576 cell(s) on the map,
192 with a flag; 17,284 half/halves a second: 14,066 unclipped, 358 plain, 2,861
subdivided, 0 past the model limit`. The 192 is the "~195 nonzero cells" the
profiling note in `docs/DEVELOPMENT.md` arrived at from the other direction.

### The object and creature walk in C#

**Mechanism measured against the recompiled routine on every call; the picture has
not been looked at, and does not need to be — every store the game's routines make
is made, with the same value.**

`patches/ModelWalk.cs` takes the pair either side of the model submit:
`func_800331B4` (the four table walks) and `func_80032588` (one model, set up and
handed to an assembler). `KF2_MODELWALK=0` is the comparison and
`KF2_MODELWALK=verify` the proof; `KF2_MODELWALK_WALK=0` and
`KF2_MODELWALK_SUBMIT=0` take one routine back on its own.

**Why this one.** `TileWalk` taught the port the static world; this is everything
that *moves*. It is not a performance change and must not be argued as one — the
numbers below say so. It is the point at which the port learns *which creature, at
what world position, in which pose, drawn by which assembler*, and like the tile
walk it was the last recompiled step in a path that is C# at both ends:
`PolyAssembler` already owns all three assemblers it dispatches to, and
`ObjectSmoothing` already carries two of the four tables it reads. `ModelWalk.Scene`
publishes the result — a `ModelDraw` per submit, carrying the kind, the slot, the
record address, the model id, the world position, the assembler and the light
record — which is the thing no consumer of GP0 can recover and which every
screen-space technique that draws the world twice needs.

**The four tables**, in the order the walk takes them, every stride and count read
straight out of the routine:

| table | base | records | stride | live when |
|---|---|---|---|---|
| creatures | `0x8016C544` | 200 | `0x7C` | `u8[+0x9] == 1` |
| objects | `0x80177714` | 396 | `0x44` | `u16[+0x6] != 0xFF` |
| effects | `0x8019CC6C` | 128 | `0x48` | `u8[+0x0] != 0xFF` |
| billboards | `0x80195174` | 128 | `0x18` | `u16[+0x0] != 0xFFFF` |

Between the first and second table, and again after the second, the walk spends two
scratch bitmaps in its own frame — texture pages wanted at `sp+0x58`, CLUTs at
`sp+0x198` — on `func_8003309C` and `func_80032FAC`. **A record that was considered
marks those bitmaps whether or not it drew**, which is why the creature loop's
bookkeeping sits outside its own draw test.

**What `func_80032588` does with a matrix of zero is the interesting half.** With a
matrix it projects the position through `RotTrans` and lights the model from *its
own* map tile; with no matrix it takes the position raw — already where it will be
drawn — and lights it from the *camera's* tile, offset by the half selector at
`0x8019953C`. That is the mechanical meaning of "welded to the screen". A second
light record blends the colour matrix, the depth cue, the light matrix and the back
colour against the first on the caller's weight, and each of the four opts out on
its own sentinel (`-1` for the matrices and the cue, `0xFF` for the back colour), so
a record can carry only the parts it wants to change. The assembler is the eleventh
stack argument: `0xFF` (and `0x80`, which also suppresses the depth bias) is
`func_8002F214`, `0xFE` is `func_80030540`, anything else is `func_8002EAEC` with
that byte as its third argument.

**Verified.** `KF2_MODELWALK=verify` runs both versions on every call from the same
registers, RAM and GTE state, compares all three, and lets the recompiled result
stand so a mismatch cannot reach the picture. Measured over `fdat05` plus areas 0,
2, 3, 5 and 6, with all four tables and all three assemblers exercised (billboards
submitting at 50-116 a second, the semi-transparent assembler at up to 288):

| routine | calls verified | RAM | registers | GTE |
|---|---|---|---|---|
| `func_800331B4`, alone | 12,287 | 1 | 0 | 0 |
| `func_80032588`, alone | 24,590 | 0 | 0 | 0 |
| both together | 16,915 / 81,106 | 2 / 0 | 0 | 0 |

**The RAM column is not zero, and what is in it is the more useful finding.** Every
one of those three mismatches is an *ambient sound*, and there were exactly as many
mismatches as there were key-ons — one, one and two. See "A verify pass replays,
it does not re-run" below.

**What it cost, which is nothing.** The frame profiler, area 1 standing still at the
autostart position, the median of about 57,000 frames, with `KF2_MODELWALK=0` and
then on. Self time, so the callees are excluded:

| | recompiled | C# | |
|---|---|---|---|
| the walk's own body | 0.0204 ms | 0.0174 ms | -15% |
| the submitter's own body | 0.0060 | 0.0060 | — |

Three microseconds a frame, which is what "not a performance change" looks like.
The submitter did not move at all, for the same reason the tile routine barely did:
almost none of its body is now C#. What is left is the recompiled libgte and the
MO blender — `func_80014FE0`, `ScaleMatrix`, `MulMatrix0`, `MulMatrix2`, the two
matrices twice, `SetColorMatrix`, `SetLightMatrix`, `func_8002DDDC`, `SetBackColor`,
`func_80015930`, `func_800158C8` and `func_80034DA8` — and the blender is the
0.04-0.14 ms a frame that `docs/DEVELOPMENT.md` names as the next largest thing in
the renderer.

`KF2_MODELWALK_PROBE=1` is the semantic tap made visible, the live slots this frame
and everything after them a rate: `this frame 21 creature, 342 object, 11 effect, 31
sprite slot(s) live, 2 model(s) submitted; a second: 288 creature, 720 object, 44
effect, 0 sprite; 1052 lit, 0 flat, 0 semi-transparent`. **The live counts are there
because a zero submit rate has two causes** — nothing in the table, or nothing the
visibility query would pass — and the billboard row was read as the first when it
was the second: 31 records live in area 1 and not one of them submitted from where
the camera stood, through a full sweep of the yaw.

### A verify pass replays, it does not re-run

**`verify` rolls back guest RAM, the CPU registers and the GTE, and nothing else.**
A routine whose subtree holds state anywhere outside those three is therefore being
*replayed*, and its second run legitimately differs. `TileWalk` never met this —
nothing under the tile walk holds port-side state, and the GP0 writes it makes land
outside the compared RAM — and `ModelWalk` met it twice, once in each direction.

**The SPU, through libsnd.** An object of kind `0x1F` is an ambient sound source: it
draws nothing, and when its retrigger interval comes due it plays through
`func_80014158` → `SsUtKeyOn`, which is recompiled MIPS that picks a voice by
reading the SPU. The SPU is the runtime's, not the guest's, so the first run leaves
a voice busy and the second run's allocator picks a different one — a 4-byte
difference in libsnd's voice table in `GAME.EXE`'s data, around `0x80073DE0`. Two
things make the attribution airtight rather than plausible: **the number of
mismatching calls equalled the number of key-ons exactly**, run after run, and
**skipping the key-on in the C# path grew that same call's difference from 4 bytes
to 65** — the whole libsnd record — which says all 65 of those bytes are
`SsUtKeyOn`'s and that only the voice index among them ever disagreed. This one is
left in place: the sound is real, it is played once either way, and the probe prints
the running key-on total next to the verify report so a lone mismatch is
attributable rather than mysterious.

**The port's own patches, through `AnimSmoothing`.** This was 15,990 mismatches of
85,643 submits, 15 bytes of posed mesh differing by one unit, and it only appeared
after warping — a creature actually mid-clip and in view. `AnimSmoothing` hooks
`func_80034DA8`, which is *inside* `func_80032588`, and it interpolates the MO clip
time from its own per-slot state rather than from guest RAM, so the second run is
handed a clock the first run already advanced. Measured: **0 of 51,525 with
`KF2_SMOOTH_ANIM=0`**, over the same route. `AnimSmoothing` now stands down while
`ModelWalk.Verifying` — the same move `PolyAssembler` makes on `EvenFog` — so the
verify is clean without the caller having to know.

**The general shape, which is worth carrying to the next one of these.** Before
trusting a `verify` count, ask what is under the routine that a RAM, register and
GTE rollback cannot reach: the SPU, the GPU, the disc, and **the port's own patches
hooked into the subtree**. The last is the easy one to miss, because it is code this
repository wrote and it looks like part of the game from the caller's side.
