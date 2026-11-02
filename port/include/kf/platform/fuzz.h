#ifndef KF_PLATFORM_FUZZ_H
#define KF_PLATFORM_FUZZ_H
// KF1's side of libverdite's fuzzer and state trace: the presses, macros, invariants and
// trace regions, set by KF_FUZZ_* and KF_TRACE. Run many at once with scripts/fuzz.py; see
// docs/FUZZING_PLAN.md and "Fuzzing" in docs/verdite-port.md.
//
//     KF_FUZZ=<seed>              on, with this seed (the game's rand too, unless KF_FUZZ_GAME_SEED)
//     KF_FUZZ_TICKS=72000         game-clock ticks (60 a second) of play before a clean exit
//     KF_FUZZ_REPLAY=<file>       press a recording back instead of the policy
//     KF_FUZZ_OUT=fuzz-input.log  where the recording goes
//     KF_FUZZ_STOP=1              exit 3 at the first violation
//     KF_FUZZ_MACROS=0            no chance macros (warp, goto, save, load), only the pad
//     KF_FUZZ_MACRORATE=100       every macro's chance, in percent
//     KF_FUZZ_WEIGHTS=walk=0,warp=20   a press's weight or a macro's chance per 10000 updates, by name
//     KF_FUZZ_SKIP=in-rock,...    invariants turned off, by name
//     KF_FUZZ_AREA=<1-5|random>   start on that floor, in an open cell
//     KF_FUZZ_RAW=3000            a raw write (a random writable field of kf.raw's layer, in range) in
//                                 this many per 10000 updates; invariants are off, so only crashes and
//                                 host_fail are findings (the raw-refused note is a chooser bug)
//     KF_FUZZ_CALL=3000           a call into the game (a random function of kf.call's layer, arguments
//                                 in range) in this many per 10000 updates; invariants are off, as with
//                                 KF_FUZZ_RAW, and a frame reached during a call is a host_fail
//     KF_FUZZ_KIT=1               start with one of every item and spells 0-8
//     KF_FUZZ_STRONG=1            start with max HP and MP 999 (not topped up)
//     KF_FUZZ_GODMODE=1           HP topped up every update
//     KF_FUZZ_LOAD=1              start by quick loading the saves folder's quick save
//     KF_FUZZ_STATUS=3600         ticks between status lines
//     KF_FUZZ_COVERAGE=fuzz-coverage.txt   floors, cells and items reached ("0": none)
//     KF_FUZZ_GAME_SEED=<n>       the game's own rand seed
//     KF_TRACE=<file>             a state trace, one line per update (with or without the fuzzer)
//     KF_FLOOR_TRACE=<file>       a trace of the floor's live state, as the handover check compares it
//     KF_FLOOR_HANDOVER=<update>  at that update, hand the floor over: capture, reload, restore
//     KF_FLOOR_DUMP=<update>      at that update, each floor trace region's bytes beside the trace
#include <kf/lib/types.h>
#include <kf/platform/input.h>
#include <array>
#include <optional>

namespace kf {
// In host_start, after the agent channel.
void fuzz_start();
// Every host poll.
void fuzz_poll();
// Game buttons held by the fuzzer, or'd into the pad.
u32 fuzz_buttons();
// A save or load macro's request, taken by the player update in place of F2/F4.
QuickRequest fuzz_quick_request();
// Between two world updates (host_between_updates): macros, invariants, the trace.
void fuzz_between_updates();
// A frame is being read or presented: a finding if a mod's call into the game is running.
void fuzz_frame_reached();
// Moves the player to the centre of a cell of this floor, as floor 5's teleport moves them; between
// updates only (the fuzzer's goto, the agent's move). False for a cell off the map.
bool player_move_to_cell(int x, int z);
// The first cell, by rows, whose four neighbours are floor too: somewhere to stand after a warp.
std::optional<std::array<int, 2>> first_open_cell();
// False under the fuzzer, which declines the system menu's Quit (a "quit-declined" line).
bool fuzz_allows_power_off();
// --compare-traces A B: prints the comparison as JSON; 0 equal, 1 different, 2 unreadable.
int fuzz_compare_traces(const char *path_a, const char *path_b);
}

#endif // KF_PLATFORM_FUZZ_H
