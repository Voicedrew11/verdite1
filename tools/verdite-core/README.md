# Verdite Core

The game-agnostic code shared by the Verdite ports, the static recompilations of
the PlayStation King's Field games built on
[the RecompOne fork](https://github.com/Voicedrew11/verdite-recompone):
[Verdite2](https://github.com/Voicedrew11/verdite2) (King's Field, `SLUS-00158`)
and [Verdite3](https://github.com/Voicedrew11/verdite3) (King's Field II in the
US, `SLUS-00255`).

Each game takes this repository as a `git subtree --squash` at
`tools/verdite-core`, pinned per game, so a fresh clone of a game builds with
nothing fetched and each game upgrades on its own schedule. An edit made inside a
game's `tools/verdite-core` is its own commit, touching nothing else, and is
pushed back here soon after (`scripts/setup_tools.sh --push-core` in the game).

**Nothing here knows a game.** Disc paths, overlay names, addresses, record
layouts, env var prefixes and settings keys come from the game, through its
`config/verdite.json` or a command-line flag. Code moves in only once a second
game needs it.

## What is here

`scripts/` holds the reverse-engineering tools a new game is brought up with.
Each reads the game's `config/verdite.json` (found by walking up from the current
directory, or from `VERDITE_GAME_ROOT`) for anything it needs to know about the
game; a flag overrides it.

| script | what |
|---|---|
| `inspect_disc.py` | the disc's ISO 9660 tree, SYSTEM.CNF and every PS-X EXE header |
| `extract_file.py` | one file off the disc, or just its EXE header |
| `add_call_targets.py` | splice the `jal` targets a linear sweep missed into a function map |
| `merge_branch_spans.py` | rejoin functions the sweep split at an interior `jr`/`j` |
| `merge_sdk_names.py` | write the PSY-Q names a signature match found into the function maps |
| `match_code.py` | a function's nearest counterparts in *another game's* executable, by structure (opcodes, GTE commands, record offsets, constants, calls), and two routines' calls aligned in order |
| `verdite_game.py` | finds the game's root and reads its `config/verdite.json` |

A game's `config/verdite.json` today:

```json
{
  "disc": "disc/KingsField2.cue",
  "recompilerConfig": "config/kf2.json",
  "funcmaps": "config/funcmaps",
  "sdkOverlays": ["open", "game", "end"]
}
```

`inspect_disc.py` and `extract_file.py` need none of it, so they run in a game
that has no config yet.

## C#

`src/` holds C# the games share. It is **source, not a library**: each game
compiles `tools/verdite-core/src/**/*.cs` into its own assembly beside its
`patches/`, so RecompOne's `HookManager` detours, reflection over the game's
types and a launcher's one Roslyn pass all see it as they see `patches/`.
Everything is in the namespace `Verdite.Core`, which each game imports with a
global using, so its callers name these types as they did their own copies.

The game says who it is once, first thing in its `Program.cs`, with
`Verdite.Core.Game.Configure(tag: "KF2")`; that gives the log prefix (`[KF2]`),
the env prefix (`KF2_`) and the lowercase id (`kf2`). Anything else core needs
from the game, it is handed at install time by the game's own code, never read
from a file.

| file | what |
|---|---|
| `Game.cs` | the game's tag, its log and env prefixes and its id |
| `HookAttach.cs` | attach on overlay loads until the pass succeeds, and read back what `HookManager` actually committed |
| `Differential.cs` | run a recompiled routine and its C# transcription from one state and compare RAM, the scratchpad, the callee-saved registers with LO/HI, and the GTE; the recompiled result stands (a game's `verify` modes) |
| `MouseIndicator.cs` | the glyph shown over the game picture when the mouse is captured or released; `Picture` is the rectangle it sits in, `OutputView` unless the game sets it |
| `KeyLayoutApply.cs` | the port's keyboard layout as a default and a once-per-version migration that leaves a customised layout alone and corrects a superseded one; the game hands it its table, `Version`, `Superseded`, the line announcing it and where the applied version is kept |
| `Kept.cs` | a setting's env var, then its saved key: the env var wins and is remembered as having won; `BoolsAsInts` for a game that keeps its switches as 0/1 |
| `Mouse.cs` | mouse look and the mouse buttons: capture, the per-frame poll, the stale-motion rule, what a tick spends and what it turned by; the game hands it a `MouseGame` (units, step cap, pitch limit, base angle addresses, default buttons, its frame clock and text-editing test) and keeps the look hook and the settings page |
| `WindowIcon.cs` | the game's memory-card icon as the window icon: `{Tag}_ICON` (`orb`, `off`, a frame), a 4bpp card icon decoded at any row stride, scaled by whole multiples to every size a desktop asks for and set at once; the game finds the icon on its disc |
| `DesktopEntry.cs` | the Wayland half of the window icon: the same sizes into `$XDG_DATA_HOME/icons/hicolor` under the app id, and a desktop entry under the game's `Name`, `GenericName` and `Comment` only when no packager wrote one; `{Tag}_ICON_INSTALL=0` writes nothing |

## The launcher

`launcher/` is the shipped executable's code, shared: a port cannot ship its game
(the recompiled code is a translation of the disc's), so it ships the inputs and
the launcher builds the game on the player's machine on first run. It settles the
data directory and chdirs into it, validates the disc, recompiles it in process,
compiles the result with the port's sources in one Roslyn pass, caches the
assembly under a key of what went into it, and hands over; it also checks GitHub
for a newer release once a day and says so (it downloads nothing).

It is **not** compiled into the game. A game's launcher project
(`Verdite2.Launcher/Verdite2.Launcher.csproj`) sets its names and imports
`launcher/Launcher.targets`, which compiles `launcher/**` into that executable and
stages the payload beside it: `config/**/*.json`, `Program.cs`, `patches/**`, this
repository's `src/**`, `mods/**`, `LICENSE` and `packaging/shared/<app id>.png`.
Its one source file hands `Launcher.Run` a `LauncherGame`:

| field | Verdite2 | Verdite3 |
|---|---|---|
| `Name`, `AppId` | `Verdite2`, `verdite2` | `Verdite3`, `verdite3` |
| `GameTitle`, `Serial` | `King's Field`, `SLUS-00158` | `King's Field II`, `SLUS-00255` |
| `WrongDiscs` | `SLUS-00255` and what to say | `SLUS-00158` and what to say |
| `RecompilerConfig`, `GameAssembly` | `kf2.json`, `KingsField2` | `kf3.json`, `KingsField3` |
| `UpdateRepository` | `Voicedrew11/verdite2` | `Voicedrew11/verdite3` |
| `PlayAfter` | `open`/`game` the title, `end`/`fdat*` play | the same |

Everything else is derived. The disc's files and their floors are read from the
recompiler config (every overlay's `file`, each at least as long as its furthest
`offset + skip + size`), so the disc check and the build key cannot drift from what
the recompile reads. The boot file comes from the serial. The env prefix
(`VERDITE2_`: `DATA`, `UPDATE_CHECK`, `BUILD`), the console tag (`[Verdite2]`),
the data folder (`%LOCALAPPDATA%\Verdite2`, `~/.local/share/verdite2`) and the
update setting's key (`Verdite2.UpdateCheck`) come from the name.

**The build options in `Build/GameCompile.cs` must match the game's csproj**, which
compiles the same sources on the developer path: a difference is a bug that
exists only in the release. Both games' csprojs set what it assumes (unsafe,
nullable, implicit usings plus `Verdite.Core`, QuickJit off in the runtimeconfig).

### Telling the player about a new release

`UpdateCheck` asks `api.github.com/repos/<UpdateRepository>/releases/latest`
(which leaves out drafts and prereleases) on a worker after the window is up, at
most once a day; `update.json` in the data directory keeps the answer and any
skipped version, and a cached answer is still announced when the network is down.
`UpdatePopup` opens once, outside play (`PlayAfter`) and never over another popup;
`UpdateBadge` puts a gold button in the menu bar until the player hides it. *Check
for updates at launch* is in Settings ▸ Interface; `<prefix>UPDATE_CHECK=0` turns
it off and `=force` ignores the daily limit. Verdite2's `docs/PACKAGING.md` has the
measurements.

## Packaging

`packaging/` builds the release from a game's checkout, with no disc:

| file | what |
|---|---|
| `linux/build-appimage.sh` | `dist/<Name>-<VERSION>-x86_64.AppImage`: the self-contained publish, `AppRun`, the desktop entry and icon, the licences |
| `windows/build-windows.ps1` | `dist/<Name>-<VERSION>-win-x64.zip` (the stub, `bin/`, `content/`, `licenses/`) and, with `iscc` on PATH, the installer |
| `windows/verdite.iss` | the Inno Setup script; every name from the environment the PowerShell script sets |
| `windows/Stub/` | the few-KB .NET Framework executable at the install root, built under the launcher's name, that starts `bin\<Name>.exe` |
| `../scripts/release.sh` | bump `VERSION`, commit, tag `v<VERSION>`; never pushes |

Each finds the game as the checkout it is vendored in (or `VERDITE_GAME_ROOT`) and
reads the game's `packaging/package.env`:

```sh
NAME=Verdite2
APP_ID=verdite2
INNO_APP_ID=9F1F0C1E-6A3E-4C69-9C2A-9E5F2B8D4A11   # one per game, never reused
```

beside `packaging/shared/<app id>.desktop`, `.png` (256×256) and `.ico`, and the
game's `VERSION`, `LICENSE` and `<Name>.Launcher/`. A game keeps one-line wrappers
at `packaging/linux/build-appimage.sh`, `packaging/windows/build-windows.ps1` and
`scripts/release.sh`, so its commands and CI do not name this path. The CI
workflows stay in each game (GitHub reads them from there), and their release body
is the game's.
