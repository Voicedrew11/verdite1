# Crash reports

When the game crashes it writes a report to `<user dir>/crashes/crash-<UTC time>-<pid>/`
(on Linux `~/.local/share/Verdite/Verdite1/crashes/`, on Windows
`%APPDATA%\Verdite\Verdite1\crashes\`) and says where on stderr (and in `kf.log`). The
mechanism is libverdite's (`verdite/crash.h`); KF1's sections, events and saves are
`port/platform/crash.cpp`.

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
- **`== events`**: the last 64 things that happened, oldest first: input context changes, floor
  changes, the player's death, save slots read and written, agent presses, mod failures.
- **`== files`**: copies of the save slots, beside `report.txt`, to start from the player's state.
- KF1's sections, read from the decomp's own state: **game** (context, language, frames, clock),
  **player** (vitals, level, gold, floor, position), **actor being updated**
  (`actor_state.current`: set only while the actor loop runs, so a fault in an enemy's update
  names the enemy, its definition, action, health and position), **last effect updated**
  (`effect_state.current_record`, which is never cleared, so it may be from an earlier update),
  and **settings** that change play or drawing.

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

On **Windows** the unhandled-exception filter and SIGABRT write the report with the stack named
in process (cpptrace with libdwarf reads the DWARF line tables the exe carries). **Not built or
run yet**: there is no MinGW toolchain on the development machine. Paths there are narrow (ANSI),
so a user folder outside the code page is not reached.

## The shipped binary carries line tables

A Release build is compiled with `-gline-tables-only` (CMakeLists.txt): enough for function
names, files, lines and inlined calls, much less than full debug info. Stripping the shipped
binary leaves reports with raw frames only.

## Testing

`verdite_tests` crashes child processes on purpose: a fault (named by the helper, from the
faulting function; a faulting section cut short, the next still written), `std::terminate` with
its message, a soft fault (named in process; the last 64 events; a kept file copied), and under
ASan a fault through the death callback. `KF_TEST_CRASH=fault|abort|soft@<gameplay polls>`
crashes the game itself once:

```bash
XDG_DATA_HOME=/tmp/kf-xdg KF_TEST_CRASH=fault@30 nix develop -c build/linux/verdite1 \
  --data data/en --saves /tmp/kf-saves --headless --turbo --skip-intro
```

Checked 2026-10-24: the report names `fault_on_purpose`, then `crash_poll`, `host_poll` and the
game's own `game_update_language_comparison` (resources.cpp:84), `render_frame`
(render_frame.cpp:85), `game_main_loop` (game.cpp:77) and `game_play` (session.cpp:77); the
abort case, report and naming together, takes under a second.

## Not yet

- A hang watchdog (the game thread's stack when it stops polling) and the focus pause.
- Soft-fault checks in the game code's patches, where a bad record would crash: report it and
  carry on as the game would.
- Replay from the input since the last load, with the fuzzer.
