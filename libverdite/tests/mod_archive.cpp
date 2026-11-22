#include <verdite/mod_archive.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace verdite;

namespace {
ModArchiveEntry file(std::string path, std::string_view text = "x") {
    return {std::move(path), std::vector<std::uint8_t>(text.begin(), text.end())};
}

std::vector<std::string> paths(const ModInstallPlan &plan) {
    std::vector<std::string> found;
    found.reserve(plan.files.size());
    for (const ModInstallFile &installed : plan.files)
        found.push_back(installed.path);
    std::ranges::sort(found);
    return found;
}
} // namespace

TEST_CASE("a mod at the top of the archive installs under its id") {
    const auto plan = mod_install_plan({file("mod.json", R"({"id":"debug","name":"Debug"})"), file("main.lua"), file("lib/x.lua")},
                                       "/downloads/debug-2.0.zip");
    CHECK(plan.refusal.empty());
    CHECK(plan.folder == "debug");
    CHECK(plan.name == "Debug");
    CHECK(paths(plan) == std::vector<std::string>{"lib/x.lua", "main.lua", "mod.json"});
}

TEST_CASE("a mod in one folder installs from inside it, and what is beside the folder stays out") {
    const auto plan = mod_install_plan({file("debug/mod.json", "{}"), file("debug/main.lua"), file("README.txt")}, "pack.zip");
    CHECK(plan.refusal.empty());
    CHECK(plan.folder == "debug");
    CHECK(plan.name == "debug");
    CHECK(paths(plan) == std::vector<std::string>{"main.lua", "mod.json"});
}

TEST_CASE("without an id or a folder, the archive's name is the folder") {
    const auto plan = mod_install_plan({file("main.lua")}, "content://x/document/primary%3ADownload%2Fquickheal.zip");
    CHECK(plan.refusal.empty());
    CHECK(plan.folder == "quickheal");
}

TEST_CASE("an id the file system cannot take falls back to the folder") {
    const auto plan = mod_install_plan({file("debug/mod.json", R"({"id":"../up"})")}, "a.zip");
    CHECK(plan.folder == "debug");
    CHECK(mod_install_plan({file("mod.json", R"({"id":".hidden"})")}, "b.zip").folder == "b");
}

TEST_CASE("a Mac's archive litter is left out") {
    const auto plan = mod_install_plan({file("m/mod.json", "{}"), file("__MACOSX/m/._mod.json"), file("m/.DS_Store")}, "m.zip");
    CHECK(plan.refusal.empty());
    CHECK(paths(plan) == std::vector<std::string>{"mod.json"});
}

TEST_CASE("an archive with no mod, or two, is refused") {
    CHECK_FALSE(mod_install_plan({file("readme.txt")}, "a.zip").refusal.empty());
    CHECK_FALSE(mod_install_plan({file("a/b/c/mod.json")}, "a.zip").refusal.empty());
    CHECK_FALSE(mod_install_plan({file("a/mod.json"), file("b/main.lua")}, "a.zip").refusal.empty());
    CHECK_FALSE(mod_install_plan({}, "a.zip").refusal.empty());
}

TEST_CASE("a path that leaves the mod's folder or that a file system refuses is refused") {
    for (const char *bad : {"../evil.lua", "a/../../evil.lua", "/etc/passwd", "a//b", "a\\b", "c:x", "con.lua", "a/LPT1",
                            "trailing.", "nul"}) {
        CAPTURE(bad);
        CHECK_FALSE(mod_install_plan({file("mod.json", "{}"), file(bad)}, "a.zip").refusal.empty());
    }
    CHECK(mod_install_plan({file("mod.json", "{}"), file("console.lua"), file("com10.txt")}, "a.zip").refusal.empty());
}

TEST_CASE("two files that differ only in case are refused") {
    CHECK_FALSE(mod_install_plan({file("mod.json", "{}"), file("a.lua"), file("A.lua")}, "a.zip").refusal.empty());
}

TEST_CASE("an archive over the limits is refused") {
    std::vector<ModArchiveEntry> many{file("mod.json", "{}")};
    for (std::size_t i = 0; i < mod_archive_files_most; ++i)
        many.push_back(file("f" + std::to_string(i)));
    CHECK_FALSE(mod_install_plan(std::move(many), "a.zip").refusal.empty());
    ModArchiveEntry big = file("big.bin");
    big.bytes.resize(mod_archive_bytes_most + 1);
    CHECK_FALSE(mod_install_plan({file("mod.json", "{}"), std::move(big)}, "a.zip").refusal.empty());
}
