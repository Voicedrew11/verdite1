#include <verdite/fuzz.h>

#include <verdite/agent.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>

namespace verdite {
namespace {

constexpr const char *recording_magic = "# verdite-fuzz 1";

struct FileCloser {
    void operator()(std::FILE *file) const { std::fclose(file); }
};
using File = std::unique_ptr<std::FILE, FileCloser>;

struct RecordedButtons {
    std::uint64_t tick;
    std::uint32_t buttons;
};
struct RecordedMacro {
    std::uint64_t update;
    std::string name, args;
};

struct Fuzz {
    FuzzConfig config;
    FuzzRandom random{1};
    bool active = false;
    File recording;
    // the policy
    bool started = false;
    std::uint64_t first_tick = 0, decided_tick = 0, hold_until = 0;
    std::uint32_t buttons = 0, recorded_buttons = 0;
    bool released = true;
    // a replay
    bool replaying = false;
    std::vector<RecordedButtons> replay_buttons;
    std::size_t replay_at = 0;
    std::deque<RecordedMacro> replay_macros;
    // the run
    std::uint64_t updates = 0, last_update_tick = 0, next_status_tick = 0;
    int settle = 0;
    bool stall_said = false, finished = false;
    std::uint64_t violations = 0;
    std::map<std::string, std::uint64_t> kinds, macro_counts, notes;
};
Fuzz fuzz;

void emit(const std::string &json) {
    std::printf("[%s-FUZZ] %s\n", fuzz.config.tag.c_str(), json.c_str());
    std::fflush(stdout);
}

std::string counts_json(const std::map<std::string, std::uint64_t> &counts) {
    std::string out = "{";
    for (const auto &[name, count] : counts)
        out += (out.size() > 1 ? "," : "") + json_quote(name) + ":" + std::to_string(count);
    return out + "}";
}

std::uint64_t played(std::uint64_t tick) { return tick - fuzz.first_tick; }

void record_line(const std::string &line) {
    if (!fuzz.recording)
        return;
    std::fputs(line.c_str(), fuzz.recording.get());
    std::fputc('\n', fuzz.recording.get());
    // A crash must not lose what led to it.
    std::fflush(fuzz.recording.get());
}

void record_buttons(std::uint64_t tick, std::uint32_t buttons) {
    if (buttons == fuzz.recorded_buttons)
        return;
    fuzz.recorded_buttons = buttons;
    char line[64];
    std::snprintf(line, sizeof line, "b %" PRIu64 " %" PRIx32, tick, buttons);
    record_line(line);
}

void finish(int exit_code) {
    if (fuzz.finished)
        return;
    fuzz.finished = true;
    emit("{\"event\":\"end\",\"code\":" + std::to_string(exit_code) + ",\"updates\":" + std::to_string(fuzz.updates) +
         ",\"violations\":" + std::to_string(fuzz.violations) + ",\"kinds\":" + counts_json(fuzz.kinds) +
         ",\"macros\":" + counts_json(fuzz.macro_counts) + ",\"notes\":" + counts_json(fuzz.notes) +
         (fuzz.config.status ? fuzz.config.status() : "") + "}");
    fuzz.recording.reset();
    if (fuzz.config.finish)
        fuzz.config.finish(exit_code);
    std::exit(exit_code);
}

void violation(std::string_view kind, std::string_view detail) {
    ++fuzz.violations;
    const auto count = ++fuzz.kinds[std::string(kind)];
    if (count <= static_cast<std::uint64_t>(fuzz.config.reports_per_kind))
        emit("{\"event\":\"violation\",\"kind\":" + json_quote(kind) + ",\"update\":" + std::to_string(fuzz.updates) +
             ",\"tick\":" + std::to_string(played(fuzz.last_update_tick)) + ",\"detail\":" + json_quote(detail) + "}");
    if (fuzz.config.stop_on_violation)
        finish(3);
}

const FuzzMacro *find_macro(std::string_view name) {
    for (const auto &macro : fuzz.config.macros_table)
        if (macro.name == name)
            return &macro;
    return nullptr;
}

// Said and recorded before it runs, so a macro that crashes the game is in the recording.
void run_macro(const FuzzMacro &macro, const std::string &args) {
    emit("{\"event\":\"macro\",\"name\":" + json_quote(macro.name) + ",\"args\":" + json_quote(args) +
         ",\"update\":" + std::to_string(fuzz.updates) + "}");
    if (!fuzz.replaying)
        record_line("m " + std::to_string(fuzz.updates) + " " + macro.name + (args.empty() ? "" : " " + args));
    if (!macro.run(args)) {
        violation("bad-macro", macro.name + " " + args);
        return;
    }
    ++fuzz.macro_counts[macro.name];
    fuzz.settle = std::max(fuzz.settle, macro.settle_updates);
}

const FuzzPress &pick(const std::vector<FuzzPress> &presses) {
    int total = 0;
    for (const auto &press : presses)
        total += std::max(press.weight, 0);
    int roll = static_cast<int>(fuzz.random.below(static_cast<std::uint32_t>(std::max(total, 1))));
    for (const auto &press : presses) {
        roll -= std::max(press.weight, 0);
        if (roll < 0)
            return press;
    }
    return presses.front();
}

// The policy's decision at one tick: a release after each press, then the next press.
void decide(std::uint64_t tick, bool in_menu) {
    if (tick < fuzz.hold_until)
        return;
    if (!fuzz.released) {
        fuzz.buttons = 0;
        fuzz.released = true;
        fuzz.hold_until = tick + static_cast<std::uint64_t>(std::max(fuzz.config.release_ticks, 1));
        return;
    }
    const auto &presses = in_menu && !fuzz.config.menu_presses.empty() ? fuzz.config.menu_presses : fuzz.config.presses;
    if (presses.empty())
        return;
    const auto &press = pick(presses);
    fuzz.buttons = press.buttons;
    fuzz.released = press.buttons == 0;
    fuzz.hold_until = tick + static_cast<std::uint64_t>(
        std::max(fuzz.random.between(press.min_hold_ticks, press.max_hold_ticks), 1));
}

// No update for a while: something modal holds the game. Press the escape buttons in
// turn, each held and then released for release_ticks, so each is an edge.
std::uint32_t escape_buttons(std::uint64_t tick) {
    if (fuzz.config.escape.empty())
        return 0;
    const auto span = static_cast<std::uint64_t>(std::max(fuzz.config.release_ticks, 1));
    const auto step = tick / (2 * span);
    return tick % (2 * span) < span ? fuzz.config.escape[step % fuzz.config.escape.size()] : 0;
}

bool stalled(std::uint64_t tick) {
    return tick - fuzz.last_update_tick > static_cast<std::uint64_t>(fuzz.config.escape_after_ticks);
}

std::uint32_t replay_buttons(std::uint64_t tick) {
    while (fuzz.replay_at < fuzz.replay_buttons.size() && fuzz.replay_buttons[fuzz.replay_at].tick <= tick)
        fuzz.buttons = fuzz.replay_buttons[fuzz.replay_at++].buttons;
    return fuzz.buttons;
}

// One line of a recording: "seed <n>", "b <tick> <hex buttons>" or "m <update> <name> [args]".
void read_recording_line(std::string_view line) {
    std::istringstream words{std::string(line)};
    std::string kind;
    std::uint64_t at = 0;
    if (!(words >> kind >> at))
        return;
    if (kind == "seed") {
        fuzz.config.seed = at;
    } else if (std::uint32_t buttons = 0; kind == "b" && (words >> std::hex >> buttons)) {
        fuzz.replay_buttons.push_back({at, buttons});
    } else if (std::string name; kind == "m" && (words >> name)) {
        std::string args;
        std::getline(words >> std::ws, args);
        fuzz.replay_macros.push_back({at, name, args});
    }
}

bool read_recording(const std::string &path) {
    std::ifstream file(path);
    if (!file)
        return false;
    bool magic = false;
    for (std::string line; std::getline(file, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (line == recording_magic)
            magic = true;
        else
            read_recording_line(line);
    }
    return magic;
}

bool open_recording() {
    if (fuzz.config.recording.empty())
        return true;
    fuzz.recording.reset(std::fopen(fuzz.config.recording.c_str(), "w"));
    if (!fuzz.recording)
        return false;
    record_line(recording_magic);
    record_line("seed " + std::to_string(fuzz.config.seed));
    for (const auto &note : fuzz.config.notes)
        record_line("note " + note);
    return true;
}

void status(std::uint64_t tick) {
    emit("{\"event\":\"status\",\"tick\":" + std::to_string(played(tick)) + ",\"updates\":" + std::to_string(fuzz.updates) +
         ",\"violations\":" + std::to_string(fuzz.violations) + ",\"macros\":" + counts_json(fuzz.macro_counts) +
         (fuzz.config.status ? fuzz.config.status() : "") + "}");
}

void check_invariants() {
    if (fuzz.settle > 0) {
        --fuzz.settle;
        return;
    }
    for (const auto &invariant : fuzz.config.invariants)
        if (auto wrong = invariant.check())
            violation(invariant.name, *wrong);
}

void play_recorded_macros() {
    while (!fuzz.replay_macros.empty() && fuzz.replay_macros.front().update <= fuzz.updates) {
        const auto recorded = fuzz.replay_macros.front();
        fuzz.replay_macros.pop_front();
        if (const auto *macro = find_macro(recorded.name))
            run_macro(*macro, recorded.args);
        else
            violation("bad-macro", "no macro " + recorded.name);
    }
}

void play_chance_macros() {
    if (!fuzz.config.macros)
        return;
    for (const auto &macro : fuzz.config.macros_table) {
        const auto chance = static_cast<std::uint32_t>(std::max(macro.per_10000 * fuzz.config.macro_rate_percent / 100, 0));
        if (chance == 0 || !fuzz.random.chance(chance))
            continue;
        if (auto args = macro.choose ? macro.choose(fuzz.random) : std::optional<std::string>(""))
            run_macro(macro, *args);
        return;
    }
}

} // namespace

FuzzRandom::FuzzRandom(std::uint64_t seed) : state_(seed) {}

std::uint64_t FuzzRandom::next() {
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

std::uint32_t FuzzRandom::below(std::uint32_t bound) { return bound == 0 ? 0 : static_cast<std::uint32_t>(next() % bound); }

int FuzzRandom::between(int lowest, int highest) {
    if (highest <= lowest)
        return lowest;
    return lowest + static_cast<int>(below(static_cast<std::uint32_t>(highest - lowest) + 1));
}

bool FuzzRandom::chance(std::uint32_t per_10000) { return below(10000) < per_10000; }

bool fuzz_start(FuzzConfig config) {
    fuzz = {};
    fuzz.config = std::move(config);
    fuzz.random = FuzzRandom(fuzz.config.seed);
    if (fuzz.config.replay) {
        if (!read_recording(*fuzz.config.replay)) {
            std::fprintf(stderr, "[%s] fuzz: cannot read the recording %s\n", fuzz.config.tag.c_str(), fuzz.config.replay->c_str());
            return false;
        }
        fuzz.replaying = true;
        fuzz.config.recording.clear();
    }
    if (!open_recording()) {
        std::fprintf(stderr, "[%s] fuzz: cannot write %s\n", fuzz.config.tag.c_str(), fuzz.config.recording.c_str());
        return false;
    }
    fuzz.active = true;
    emit("{\"event\":\"start\",\"seed\":" + std::to_string(fuzz.config.seed) + ",\"ticks\":" + std::to_string(fuzz.config.ticks) +
         ",\"ticksPerSecond\":" + std::to_string(fuzz.config.ticks_per_second) +
         ",\"replay\":" + (fuzz.replaying ? "true" : "false") + "}");
    return true;
}

bool fuzz_active() { return fuzz.active; }

bool fuzz_replaying() { return fuzz.active && fuzz.replaying; }

std::uint64_t fuzz_seed() { return fuzz.config.seed; }

void fuzz_poll(std::uint64_t tick) {
    if (!fuzz.active || fuzz.finished)
        return;
    if (!fuzz.started) {
        fuzz.started = true;
        fuzz.first_tick = fuzz.decided_tick = fuzz.last_update_tick = tick;
        fuzz.next_status_tick = tick + static_cast<std::uint64_t>(fuzz.config.status_every_ticks);
    }
    if (fuzz.config.status_every_ticks > 0 && tick >= fuzz.next_status_tick) {
        fuzz.next_status_tick = tick + static_cast<std::uint64_t>(fuzz.config.status_every_ticks);
        status(tick);
    }
    if (!fuzz.stall_said && tick - fuzz.last_update_tick > static_cast<std::uint64_t>(fuzz.config.stall_after_ticks)) {
        fuzz.stall_said = true;
        violation("stall", "no update for " + std::to_string(tick - fuzz.last_update_tick) + " ticks");
    }
    if (played(tick) >= fuzz.config.ticks)
        finish(0);
}

std::uint32_t fuzz_buttons(std::uint64_t tick) {
    if (!fuzz.active || !fuzz.started)
        return 0;
    if (fuzz.replaying)
        return replay_buttons(tick);
    if (stalled(tick)) {
        fuzz.decided_tick = fuzz.hold_until = tick;
        fuzz.released = true;
        fuzz.buttons = escape_buttons(tick);
    } else {
        const bool in_menu = fuzz.config.in_menu && fuzz.config.in_menu();
        while (fuzz.decided_tick < tick)
            decide(++fuzz.decided_tick, in_menu);
    }
    record_buttons(tick, fuzz.buttons);
    return fuzz.buttons;
}

void fuzz_between_updates(std::uint64_t tick) {
    if (!fuzz.active || !fuzz.started)
        return;
    ++fuzz.updates;
    fuzz.last_update_tick = tick;
    fuzz.stall_said = false;
    if (fuzz.replaying)
        play_recorded_macros();
    else
        play_chance_macros();
    check_invariants();
}

void fuzz_trigger(std::string_view macro, std::string_view args) {
    if (!fuzz.active || fuzz.replaying)
        return;
    if (const auto *found = find_macro(macro))
        run_macro(*found, std::string(args));
    else
        violation("bad-macro", "no macro " + std::string(macro));
}

void fuzz_settle(int updates) { fuzz.settle = std::max(fuzz.settle, updates); }

void fuzz_note(std::string_view what) {
    if (!fuzz.active)
        return;
    ++fuzz.notes[std::string(what)];
    emit("{\"event\":\"note\",\"what\":" + json_quote(what) + ",\"update\":" + std::to_string(fuzz.updates) + "}");
}

void fuzz_report(std::string_view kind, std::string_view detail) {
    if (fuzz.active)
        violation(kind, detail);
}

} // namespace verdite
