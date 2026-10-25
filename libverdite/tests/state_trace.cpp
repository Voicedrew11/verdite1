#include <verdite/state_trace.h>

#include <doctest/doctest.h>

#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <fstream>

using namespace verdite;
namespace fs = std::filesystem;

namespace {

fs::path temp_path(const std::string &name) {
    return fs::temp_directory_path() / ("verdite-trace-" + std::to_string(::getpid()) + "-" + name);
}

struct World {
    int hp = 30;
    int position = 0;
};

std::vector<TraceRegion> regions(const World &world) {
    auto value = [](const int &field) {
        return [&field](std::vector<std::byte> &out) {
            const auto *bytes = reinterpret_cast<const std::byte *>(&field);
            out.insert(out.end(), bytes, bytes + sizeof field);
        };
    };
    return {{"hp", value(world.hp)}, {"position", value(world.position)}};
}

// Ten updates; at update `change` the position moves one further than usual.
void write_trace(const fs::path &path, int change) {
    World world;
    auto trace = StateTrace::create(path.string(), regions(world));
    if (!trace) {
        FAIL("cannot write " << path);
        return;
    }
    for (int update = 1; update <= 10; ++update) {
        world.position += update == change ? 2 : 1;
        trace->record(static_cast<std::uint64_t>(update), static_cast<std::uint64_t>(update) * 4);
    }
}

} // namespace

TEST_CASE("trace_hash is FNV-1a") {
    CHECK(trace_hash({}) == 0xCBF29CE484222325ull);
    const char a = 'a';
    CHECK(trace_hash(std::as_bytes(std::span(&a, 1))) == 0xAF63DC4C8601EC8Cull);
}

TEST_CASE("equal runs compare equal; a change is found at its update and region") {
    const auto a = temp_path("a"), b = temp_path("b"), c = temp_path("c");
    write_trace(a, 0);
    write_trace(b, 0);
    write_trace(c, 7);
    const auto same = trace_compare(a.string(), b.string());
    const auto differ = trace_compare(a.string(), c.string());
    if (!same || !differ || !differ->divergence) {
        FAIL("the traces did not compare");
        return;
    }
    CHECK(same->updates == 10);
    CHECK_FALSE(same->divergence);
    const auto &found = *differ->divergence;
    CHECK(found.update == 7);
    CHECK(found.region == "position");
    CHECK(found.tick_a == 28);
    fs::remove(a);
    fs::remove(b);
    fs::remove(c);
}

TEST_CASE("traces of different regions, or no trace, do not compare") {
    const auto a = temp_path("ra"), other = temp_path("other");
    write_trace(a, 0);
    std::ofstream(other) << "# verdite-trace 1\nregions hp\n1 4 0000000000000000\n";
    CHECK_FALSE(trace_compare(a.string(), other.string()));
    CHECK_FALSE(trace_compare(a.string(), temp_path("missing").string()));
    fs::remove(a);
    fs::remove(other);
}
