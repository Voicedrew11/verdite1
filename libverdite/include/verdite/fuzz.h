#ifndef VERDITE_FUZZ_H
#define VERDITE_FUZZ_H
// A monkey at the pad: seeded random play stepped on game-clock ticks, jumps (macros) now
// and then, invariants checked between the game's updates, and a recording that replays the
// run. Everything it finds is a "[<tag>-FUZZ] {...}" line on stdout for scripts/fuzz.py.
// Game-agnostic: the game names its presses, macros and invariants, and says when it is in a
// menu. See docs/FUZZING_PLAN.md.
//
// All of it runs on the game thread. The clock is the game's own tick (any fixed rate), which
// under turbo moves only with the game's waits, so a seed or a recording plays the same run
// again as long as the game is deterministic.
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace verdite {

// The policy's random numbers (SplitMix64), apart from the game's own.
class FuzzRandom {
  public:
    explicit FuzzRandom(std::uint64_t seed);
    std::uint64_t next();
    // 0 .. bound-1; 0 when bound is 0.
    std::uint32_t below(std::uint32_t bound);
    // lowest .. highest, both included.
    int between(int lowest, int highest);
    bool chance(std::uint32_t per_10000);

  private:
    std::uint64_t state_;
};

// One thing the policy may press: game buttons held for a while, chosen by weight.
struct FuzzPress {
    std::string name;          // "walk", "attack"
    std::uint32_t buttons;     // the game's button mask; 0 holds nothing
    int weight;
    int min_hold_ticks, max_hold_ticks;
};

// A jump the pad would take too long to make (a warp, a load). choose picks its arguments,
// or nothing when it cannot run now; run does it with them and says whether they were good.
// Arguments are one line of text, so a recording replays the macro exactly.
struct FuzzMacro {
    std::string name;          // "warp"
    int per_10000;             // chance per update while playing; 0: only when triggered
    std::function<std::optional<std::string>(FuzzRandom &random)> choose;
    std::function<bool(std::string_view args)> run;
    int settle_updates = 60;   // invariants rest this long after it
};

// Something that must hold between updates. check returns what is wrong, or nothing.
struct FuzzInvariant {
    std::string name;          // "hp-over-max"
    std::function<std::optional<std::string>()> check;
};

struct FuzzConfig {
    std::string tag;                     // "KF1": lines are "[KF1-FUZZ] {...}"
    std::uint64_t seed = 1;              // a replay takes its recording's
    std::uint64_t ticks = 20000;         // of play; then finish(0)
    int ticks_per_second = 60;           // for the report only
    std::optional<std::string> replay;   // a recording to press back instead of the policy
    std::string recording = "fuzz-input.log";
    std::vector<std::string> notes;      // "KEY=value" lines for the recording's header
    bool stop_on_violation = false;      // finish(3) at the first
    bool macros = true;                  // false: chance macros off, triggered ones still run
    int macro_rate_percent = 100;        // scales every macro's per_10000
    int release_ticks = 5;               // nothing held between presses, so each is an edge
    int escape_after_ticks = 600;        // no update this long: press escape to leave a modal loop
    int stall_after_ticks = 6000;        // no update this long: a "stall" violation
    int status_every_ticks = 3600;
    int reports_per_kind = 5;            // violation lines per kind; the rest are only counted
    std::vector<FuzzPress> presses;      // while playing
    std::vector<FuzzPress> menu_presses; // while in_menu() says so
    std::vector<std::uint32_t> escape;   // pressed in turn, with releases, during a stall
    std::vector<FuzzMacro> macros_table;
    std::vector<FuzzInvariant> invariants;
    std::function<bool()> in_menu;
    std::function<std::string()> status; // extra fields for status lines: "" or ',"hp":30,...'
    // Ends the process with this code: 0 done, 3 stopped at a violation. Must not return.
    std::function<void(int exit_code)> finish;
};

// On the game thread, before play. False when the replay or the recording cannot be opened
// (a line says why); the run is then off.
bool fuzz_start(FuzzConfig config);
bool fuzz_active();
bool fuzz_replaying();
// The run's seed: the config's, or a replayed recording's.
std::uint64_t fuzz_seed();
// Every host poll, with the game clock: ends the run at config.ticks, writes status lines,
// notices a stall.
void fuzz_poll(std::uint64_t tick);
// The buttons held at this tick, from the policy or the recording. Every input read.
std::uint32_t fuzz_buttons(std::uint64_t tick);
// Between two of the game's updates, where its state is whole: runs a due macro (the policy's
// or the recording's), then the invariants unless settling.
void fuzz_between_updates(std::uint64_t tick);
// Runs a macro now with these arguments and records it (a start position, an unstick). Ignored
// while replaying, since the recording already holds it.
void fuzz_trigger(std::string_view macro, std::string_view args);
// No invariant checks for this many updates (after a death, a load, a floor change).
void fuzz_settle(int updates);
// Something worth a line that is not a finding ("quit-declined"); counted in the end line.
void fuzz_note(std::string_view what);
// A finding the game makes itself (a failed check of its own): one violation line.
void fuzz_report(std::string_view kind, std::string_view detail);

} // namespace verdite

#endif // VERDITE_FUZZ_H
