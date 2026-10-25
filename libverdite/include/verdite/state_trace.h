#ifndef VERDITE_STATE_TRACE_H
#define VERDITE_STATE_TRACE_H
// A hash of the game's state at each of its updates, written to a file, so two runs of one
// recording can be compared: the first update and region where they part. Game-agnostic: the
// game names its regions and writes each as it should compare, pointers as indices into their
// tables, so two builds' traces compare. See docs/FUZZING_PLAN.md.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace verdite {

struct TraceRegion {
    std::string name;   // "player_state", "actors"
    // Appends the region's bytes as they should compare.
    std::function<void(std::vector<std::byte> &out)> write;
};

// 64-bit FNV-1a.
std::uint64_t trace_hash(std::span<const std::byte> bytes);

class StateTrace {
  public:
    // Writes the header (the region names); nothing when path cannot be written.
    static std::optional<StateTrace> create(const std::string &path, std::vector<TraceRegion> regions);
    // One line: the update's number, its game tick and a hash per region.
    void record(std::uint64_t update, std::uint64_t tick);

  private:
    struct Closer { void operator()(std::FILE *file) const; };
    StateTrace(std::unique_ptr<std::FILE, Closer> file, std::vector<TraceRegion> regions);
    std::unique_ptr<std::FILE, Closer> file_;
    std::vector<TraceRegion> regions_;
    std::vector<std::byte> buffer_;
};

struct TraceDivergence {
    std::uint64_t update;       // the first update that differs
    std::uint64_t tick_a, tick_b;
    std::string region;         // its first region that differs; "tick" when only the ticks do
};
struct TraceComparison {
    std::uint64_t updates;      // compared: as many as the shorter trace holds
    std::optional<TraceDivergence> divergence;
};
// Nothing when either file cannot be read or their regions are not the same.
std::optional<TraceComparison> trace_compare(const std::string &path_a, const std::string &path_b);

} // namespace verdite

#endif // VERDITE_STATE_TRACE_H
