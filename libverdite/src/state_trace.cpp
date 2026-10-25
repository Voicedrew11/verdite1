#include <verdite/state_trace.h>

#include <cinttypes>
#include <cstring>
#include <fstream>
#include <sstream>

namespace verdite {
namespace {

constexpr const char *trace_magic = "# verdite-trace 1";

struct TraceLine {
    std::uint64_t update = 0, tick = 0;
    std::vector<std::uint64_t> hashes;
};

struct TraceReader {
    std::ifstream file;
    std::vector<std::string> regions;

    bool open(const std::string &path) {
        file.open(path);
        std::string magic, names;
        if (!std::getline(file, magic) || magic != trace_magic || !std::getline(file, names) ||
            names.rfind("regions", 0) != 0)
            return false;
        std::istringstream words(names.substr(7));
        for (std::string name; words >> name;)
            regions.push_back(name);
        return !regions.empty();
    }

    std::optional<TraceLine> next() {
        std::string text;
        if (!std::getline(file, text))
            return std::nullopt;
        std::istringstream words(text);
        TraceLine line;
        if (!(words >> line.update >> line.tick))
            return std::nullopt;
        for (std::string hash; words >> hash;)
            line.hashes.push_back(std::strtoull(hash.c_str(), nullptr, 16));
        if (line.hashes.size() != regions.size())
            return std::nullopt;
        return line;
    }
};

} // namespace

std::uint64_t trace_hash(std::span<const std::byte> bytes) {
    std::uint64_t hash = 0xCBF29CE484222325ull;
    for (const auto byte : bytes) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= 0x100000001B3ull;
    }
    return hash;
}

void StateTrace::Closer::operator()(std::FILE *file) const { std::fclose(file); }

StateTrace::StateTrace(std::unique_ptr<std::FILE, Closer> file, std::vector<TraceRegion> regions)
    : file_(std::move(file)), regions_(std::move(regions)) {}

std::optional<StateTrace> StateTrace::create(const std::string &path, std::vector<TraceRegion> regions) {
    std::unique_ptr<std::FILE, Closer> file(std::fopen(path.c_str(), "w"));
    if (!file || regions.empty())
        return std::nullopt;
    std::fprintf(file.get(), "%s\nregions", trace_magic);
    for (const auto &region : regions)
        std::fprintf(file.get(), " %s", region.name.c_str());
    std::fputc('\n', file.get());
    return StateTrace(std::move(file), std::move(regions));
}

void StateTrace::record(std::uint64_t update, std::uint64_t tick) {
    std::fprintf(file_.get(), "%" PRIu64 " %" PRIu64, update, tick);
    for (const auto &region : regions_) {
        buffer_.clear();
        region.write(buffer_);
        std::fprintf(file_.get(), " %016" PRIx64, trace_hash(buffer_));
    }
    std::fputc('\n', file_.get());
    // A crash must not lose the updates before it.
    std::fflush(file_.get());
}

std::optional<TraceComparison> trace_compare(const std::string &path_a, const std::string &path_b) {
    TraceReader a, b;
    if (!a.open(path_a) || !b.open(path_b) || a.regions != b.regions)
        return std::nullopt;
    TraceComparison comparison{0, std::nullopt};
    for (;;) {
        const auto line_a = a.next(), line_b = b.next();
        if (!line_a || !line_b)
            return comparison;
        ++comparison.updates;
        for (std::size_t i = 0; i < a.regions.size(); ++i)
            if (line_a->hashes[i] != line_b->hashes[i]) {
                comparison.divergence = TraceDivergence{line_a->update, line_a->tick, line_b->tick, a.regions[i]};
                return comparison;
            }
        if (line_a->update != line_b->update || line_a->tick != line_b->tick) {
            comparison.divergence = TraceDivergence{line_a->update, line_a->tick, line_b->tick, "tick"};
            return comparison;
        }
    }
}

} // namespace verdite
