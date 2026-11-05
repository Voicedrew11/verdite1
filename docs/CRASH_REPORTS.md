# Crash reports

When the game crashes it writes a report to `<user dir>/crashes/crash-<UTC time>-<pid>/`
(on Linux `~/.local/share/Verdite/Verdite1/crashes/`, on Windows
`%APPDATA%\Verdite\Verdite1\crashes\`), says where on stderr (and in `kf.log`), and tells the
player. A hang writes one to `hang-<UTC time>-<pid>/`. The newest 20 are kept. The mechanism is
libverdite's (`verdite/crash.h`); KF1's sections, events, saves and what the player is told are
`port/platform/crash.cpp`.

## What the player sees

As Verdite2 and Verdite3: the game's picture goes and a window says "The game stopped", where the
report is, with **Open the report's folder** and **Quit**.

- **Desktop, in the crash.** A process that has faulted cannot draw, so it starts itself again as
  `--verdite-crash-window <folder> <exit code>`. On POSIX that replaces the crashed process
  (`execv`): the game's window and mouse grab go, the pid is the same, and an AppImage's runtime
  still has its child, so the mount the window runs from stays. It ends with the code the crash
  would have (128 + signal). On Windows it is a new process and the crashed one ends. Not in a
  headless run; `KF_CRASH_WINDOW=0` turns it off.
- **The next launch.** A report is born with an `unseen` file beside it, removed when the player is
  told. If the window could not be shown (no display, the process killed) or there is no window to
  start, the next launch says the game stopped last time, with Continue in place of Quit.
- **Android.** The game is a library in the system's process and cannot start itself, so a crash
  ends with whatever the system shows, and the next launch has the dialog. A phone has no folder a
  player can open, so its button is **Share report**: the report and the end of `kf.log` through the
  system's share sheet.

**Nothing is turned off when it fails.** Verdite2 and Verdite3 catch an exception from a patch's hook,
turn that patch off and carry on. Here a fault in a patch is a crash, with the window: a game running
without a patch it was meant to have is worse than one that says it stopped. `host_fail` (an error the
game cannot go on from) writes a report too, and its box says what went wrong. A Lua mod is another
matter: it is someone else's code, so one that errs is stopped, with its error in the Mods window, the
console and the report's events, and the game goes on (`docs/MODDING.md`).

## Hangs

`crash_hang_beat` runs on every host poll. A thread of its own counts its ticks without one (15 s;
`KF_HANG=seconds`, 0 for off) and asks for a report. Ticks and not the clock, so a machine that was
asleep has not been hanging. It waits for the first beat, so first-launch setup and its dialogs do
not count; `crash_hang_pause` holds it around `host_fail`'s box and while a phone has the app in the
background. Off in a headless run unless `KF_HANG` is set, as the fuzzer has its own timeouts. Never
under a debugger.

The stack is the game thread's own. On POSIX the watch sends it SIGUSR2 and its handler writes the
report as a crash's handler does, so the game's state is read by the thread that owns it; if it does
not answer in 2 s the report says so, without a stack. On Windows the watch suspends the thread and
reads its context. Either way the game is not stopped: if beats come back, the report gets
`== recovered`, the `unseen` file goes and the player is not told. Dragging a window on Windows holds
the event pump, so a long drag can write one of those.

`KF_TEST_CRASH=hang@30` spins for good. Under `nix develop -c`, end it by its own pid: `timeout` around
`nix develop` leaves it running.

## What a report holds

`report.txt`, in this order:

- **The header**: the build (version, commit with `+edits` for a dirty tree, build type), the
  binary, pid, time, whether the game's thread faulted, the fault (signal or Windows exception)
  and the address it touched, and what `std::terminate` was handed.
- **`== stack`**: the stack named to functions, files and lines, inlined calls included, from the
  faulting function down. Game code is named at its file in `build/<preset>/decomp_src/`; the
  same path below `work/` is the one to edit (`AGENTS.md`).
- **`== frames`**: the raw frames, `#n <address> <address in its object> <object>`. If the stack
  could not be named, `llvm-symbolizer --obj=<object> <address in its object>` names a frame
  by hand, against that exact binary.
- **`== registers`**: the faulting or hung thread's.
- **`== events`**: the last 64 things that happened, oldest first: input context changes, floor
  changes, the player's death, save slots read and written, agent presses, mod failures.
- **`== files`**: copies of the save slots and `kf.log`, beside `report.txt`: the player's state to
  start from, and the log the next launch would overwrite.
- KF1's sections, read from the decomp's own state: **game** (context, language, frames, clock),
  **player** (vitals, level, gold, floor, position), **actor being updated**
  (`actor_state.current`: set only while the actor loop runs, so a fault in an enemy's update
  names the enemy, its definition, action, health and position), **last effect updated**
  (`effect_state.current_record`, which is never cleared, so it may be from an earlier update),
  **settings** that change play or drawing, and **environment**, the `KF_*` variables that are set.

Verdite2's and Verdite3's reports also hold the last indirect calls, the overlays and 2 MB of the
PlayStation's RAM: none exists in a native build, where the sections read the game's state by name.

## How it works

The handler (fatal signals on POSIX, on its own stack so a stack overflow is caught) may not
allocate or lock, so it writes only what it can without: cpptrace's raw frames and their objects,
the events, the copies and the sections, through `CrashWriter`, a fixed buffer. A section reads
plain state and bounds-checks every pointer (an actor pointer is checked against the actor
array, not followed); if it faults anyway, the handler jumps back, marks the section `(stopped:
reading this state faulted)` and goes on to the next.

Then it starts the same binary again with `--verdite-crash-helper <report>`, which `main` hands
to `verdite::crash_helper_run` before anything else. The helper reads the frames back, names them
from the line tables and puts the named stack in place. The crashed process waits for it (30 s
at most), so its image is still at `/proc/<pid>/exe`: the helper reads that, not the path, and a
binary rebuilt since the game started cannot give wrong names. Then the default action runs (a
core dump where they are on, exit code 128 + signal).

`std::terminate` keeps the exception's `what()` and aborts into the handler. A **soft fault**
(`crash_report_soft`: something the game survives) writes the same report with the stack named
in process and returns; at most 8 a run.

Under **AddressSanitizer** (`--preset sanitize`) the sanitizer reports faults itself: the report
is written from its death callback, without a stack (the sanitizer's own is in the output),
and only SIGILL, SIGTRAP and SIGABRT go to our handler.

On **Windows** the unhandled-exception filter and SIGABRT walk the stack from the thread's context
(`RtlVirtualUnwind`, which allocates nothing), write the report with the frames last, as walking a
damaged stack can fault, and start the helper as on POSIX; it adds the exe's preferred base to the
frames, so a frame named by hand is `llvm-symbolizer --obj=verdite1.exe <base + address in its
object>`. Report paths are wide, so a user folder outside the code page is reached. Checked under
Wine 2026-11-05 with the cross-built package: a fault and a hang, with files and lines; a function
whose name is in `??` still has its file and line. Not yet on a real Windows.

On **Android** cpptrace can take no stack in a signal handler at all, so the frame pointers are
walked (the build keeps them, CMakeLists.txt) and objects found with `dladdr`. The report keeps
addresses, as there is no helper; the release's `-symbols.zip` names them off the phone:

```bash
scripts/name-android-report.py report.txt Verdite1-0.1.0-android-arm64-symbols.zip
```

`VERDITE_CRASH_FRAME_POINTERS` builds the walker on a desktop, to test it. The emulator does not run
on the development machine (`ANDROID_PLAN.md`), so on a phone it is not yet run.

## The shipped binary carries line tables

A Release build is compiled with `-gline-tables-only` (CMakeLists.txt): enough for function
names, files, lines and inlined calls, much less than full debug info. Stripping the shipped
binary leaves reports with raw frames only.

## Testing

`verdite_tests` crashes child processes on purpose: a fault (named by the helper, from the
faulting function; a faulting section cut short, the next still written), `std::terminate` with
its message, a soft fault (named in process; the last 64 events; a kept file copied), under
ASan a fault through the death callback, a crash becoming the window, a hang (its stack and state),
a hang that ends, a pause, and which reports are kept. `KF_TEST_CRASH=fault|abort|soft|hang@<gameplay
polls>` crashes the game itself once:

```bash
XDG_DATA_HOME=/tmp/kf-xdg KF_TEST_CRASH=fault@30 nix develop -c build/linux/verdite1 \
  --data data/en --saves /tmp/kf-saves --headless --turbo --skip-intro
```

Checked 2026-10-24: the report names `fault_on_purpose`, then `crash_poll`, `host_poll` and the
game's own `game_update_language_comparison` (resources.cpp:84), `render_frame`
(render_frame.cpp:85), `game_main_loop` (game.cpp:77) and `game_play` (session.cpp:77); the
abort case, report and naming together, takes under a second.

## Not yet

- The focus pause.
- Soft-fault checks in the game code's patches, where a bad record would crash: report it and
  carry on as the game would.
- Replay from the input since the last load, with the fuzzer.
