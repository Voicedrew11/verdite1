#include <verdite/content_ids.h>

#include <doctest/doctest.h>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

using verdite::ContentIdRange;
using verdite::ContentIds;

namespace {
constexpr ContentIdRange items{160, 3};

std::filesystem::path scratch_file() {
    const auto dir = std::filesystem::temp_directory_path() / ("verdite_content_ids_" + std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    const auto path = dir / "ids";
    std::filesystem::remove(path);
    return path;
}
} // namespace

TEST_CASE("a key keeps its number from run to run") {
    const auto path = scratch_file();
    {
        ContentIds ids(path);
        CHECK(ids.assign("item", "a/ring", items) == 160u);
        CHECK(ids.assign("item", "a/ring", items) == 160u);
        CHECK(ids.assign("item", "a/staff", items) == 161u);
    }
    ContentIds again(path);
    CHECK(again.find("item", "a/staff") == 161u);
    CHECK(again.assign("item", "a/ring", items) == 160u);
    CHECK(again.assign("item", "b/new", items) == 162u);
}

TEST_CASE("each kind counts on its own, and a range runs out") {
    ContentIds ids(scratch_file());
    CHECK(ids.assign("floor", "x", ContentIdRange{6, 1}) == 6u);
    CHECK_FALSE(ids.assign("floor", "y", ContentIdRange{6, 1}));
    CHECK(ids.assign("item", "x", items) == 160u);
}

TEST_CASE("a key that is not a plain name gets no number") {
    ContentIds ids(scratch_file());
    CHECK_FALSE(ids.assign("item", "", items));
    CHECK_FALSE(ids.assign("item", "two words", items));
    CHECK_FALSE(ids.assign("item", "line\nbreak", items));
    CHECK_FALSE(ids.assign("item", std::string(65, 'a'), items));
    CHECK_FALSE(ids.assign("kind kind", "x", items));
}

TEST_CASE("a number outside the range asked for is refused, not moved") {
    const auto path = scratch_file();
    {
        std::ofstream out(path);
        out << "item ring 12\n";
    }
    ContentIds ids(path);
    CHECK_FALSE(ids.assign("item", "ring", items));
}

TEST_CASE("a damaged file costs lines and never gives a number twice") {
    const auto path = scratch_file();
    {
        std::ofstream out(path);
        out << "nonsense\nitem\nitem ring\nitem bad name 3\nitem ok notanumber\nitem old 160\n\n";
    }
    ContentIds ids(path);
    CHECK(ids.find("item", "old") == 160u);
    CHECK_FALSE(ids.find("item", "ring"));
    CHECK(ids.assign("item", "new", items) == 161u);
}

TEST_CASE("a file that cannot be written gives no number") {
    ContentIds ids(std::filesystem::temp_directory_path() / "verdite_no_such_dir_at_all" / "ids");
    CHECK_FALSE(ids.assign("item", "x", items));
}
