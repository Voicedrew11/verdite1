#include <verdite/mod_saved.h>

#include <doctest/doctest.h>

#include <string>
#include <utility>
#include <vector>

using namespace verdite;

namespace {

SavedMod mod(std::string id, std::string name, std::string version) {
    return {std::move(id), std::move(name), std::move(version)};
}

// The list a text decodes to, which must be one.
std::vector<SavedMod> decoded_list(std::string_view json) {
    const auto decoded = saved_mods_decode(json);
    REQUIRE(decoded.has_value());
    return decoded.value_or(std::vector<SavedMod>{});
}

// Whether text decodes to nothing, the way every bad save is refused.
bool refused(std::string_view text) {
    return !saved_mods_decode(text).has_value();
}

} // namespace

TEST_CASE("a save's mods encode to JSON and decode back the same") {
    const std::vector<SavedMod> mods = {mod("speed", "Speed", "1.2"), mod("crypt", "Ember Crypt", "")};
    const std::string text = saved_mods_encode(mods);
    CHECK(text == R"({"mods":[{"id":"speed","name":"Speed","version":"1.2"},{"id":"crypt","name":"Ember Crypt","version":""}]})");
    CHECK(decoded_list(text) == mods);
}

TEST_CASE("an empty list is a save made with no mods, and round trips") {
    const std::string text = saved_mods_encode({});
    CHECK(text == R"({"mods":[]})");
    CHECK(decoded_list(text).empty());
}

TEST_CASE("a name missing or not a string falls back to the id, and a version to empty") {
    const auto decoded = decoded_list(
        R"({"mods":[{"id":"a"},{"id":"b","name":3,"version":{}},{"id":"c","name":"C","version":"2"}]})");
    REQUIRE(decoded.size() == 3);
    CHECK(decoded[0] == mod("a", "a", ""));
    CHECK(decoded[1] == mod("b", "b", ""));
    CHECK(decoded[2] == mod("c", "C", "2"));
}

TEST_CASE("a save that is not an object with a mods array is refused") {
    CHECK(refused(""));
    CHECK(refused("not json"));
    CHECK(refused("[]"));
    CHECK(refused("3"));
    CHECK(refused(R"({})"));
    CHECK(refused(R"({"mods":{}})"));
    CHECK(refused(R"({"mods":"a"})"));
    CHECK(refused(R"({"mods":null})"));
}

TEST_CASE("an entry that is not an object, or has no non-empty id, is refused") {
    CHECK(refused(R"({"mods":["a"]})"));
    CHECK(refused(R"({"mods":[null]})"));
    CHECK(refused(R"({"mods":[{"name":"A"}]})"));
    CHECK(refused(R"({"mods":[{"id":""}]})"));
    CHECK(refused(R"({"mods":[{"id":3}]})"));
    CHECK(refused(R"({"mods":[{"id":["a"]}]})"));
}

TEST_CASE("two entries with one id are refused") {
    CHECK(refused(R"({"mods":[{"id":"a"},{"id":"a","name":"again"}]})"));
    CHECK(saved_mods_decode(R"({"mods":[{"id":"a"},{"id":"b"}]})").has_value());
}

TEST_CASE("more than saved_mods_most entries is refused, and exactly that many is not") {
    std::vector<SavedMod> full;
    full.reserve(saved_mods_most + 1);
    for (std::size_t i = 0; i < saved_mods_most; ++i)
        full.push_back(mod("m" + std::to_string(i), "", ""));
    CHECK(decoded_list(saved_mods_encode(full)).size() == saved_mods_most);

    full.push_back(mod("one-more", "", ""));
    CHECK(refused(saved_mods_encode(full)));
}

TEST_CASE("compare: identical lists differ in nothing") {
    const std::vector<SavedMod> mods = {mod("a", "A", "1"), mod("b", "B", "")};
    CHECK(saved_mods_compare(mods, mods).empty());
    CHECK(saved_mods_compare({}, {}).empty());
}

TEST_CASE("compare: a mod in the save and not running is Missing, with the save's copy") {
    const std::vector<SavedMod> in_save = {mod("a", "A", "1")};
    const auto differences = saved_mods_compare(in_save, {});
    REQUIRE(differences.size() == 1);
    CHECK(differences[0].change == SavedModChange::Missing);
    CHECK(differences[0].mod == mod("a", "A", "1"));
}

TEST_CASE("compare: a mod running and not in the save is Added, with the running copy") {
    const std::vector<SavedMod> running = {mod("a", "A", "2")};
    const auto differences = saved_mods_compare({}, running);
    REQUIRE(differences.size() == 1);
    CHECK(differences[0].change == SavedModChange::Added);
    CHECK(differences[0].mod == mod("a", "A", "2"));
}

TEST_CASE("compare: a mod in both at different versions is OtherVersion, with the running copy") {
    const std::vector<SavedMod> in_save = {mod("a", "A", "1")};
    const std::vector<SavedMod> running = {mod("a", "A renamed", "2")};
    const auto differences = saved_mods_compare(in_save, running);
    REQUIRE(differences.size() == 1);
    CHECK(differences[0].change == SavedModChange::OtherVersion);
    CHECK(differences[0].mod == mod("a", "A renamed", "2"));
}

TEST_CASE("compare: mods match by id, not by name") {
    const std::vector<SavedMod> in_save = {mod("a", "Same", "")};
    const std::vector<SavedMod> running = {mod("b", "Same", "")};
    const auto differences = saved_mods_compare(in_save, running);
    REQUIRE(differences.size() == 2);
    CHECK(differences[0].change == SavedModChange::Missing);
    CHECK(differences[1].change == SavedModChange::Added);
}

TEST_CASE("compare: sorted by change in the enum's order, then by name") {
    const std::vector<SavedMod> in_save = {mod("z", "Zed", "1"), mod("m", "Mid", "1"), mod("gone-b", "Beta", "")};
    const std::vector<SavedMod> running = {mod("z", "Zed", "2"), mod("m", "Mid", "2"), mod("new-a", "Alpha", ""),
                                           mod("new-c", "Gamma", "")};
    const auto differences = saved_mods_compare(in_save, running);
    REQUIRE(differences.size() == 5);
    CHECK(differences[0] == SavedModDifference{mod("gone-b", "Beta", ""), SavedModChange::Missing});
    CHECK(differences[1] == SavedModDifference{mod("new-a", "Alpha", ""), SavedModChange::Added});
    CHECK(differences[2] == SavedModDifference{mod("new-c", "Gamma", ""), SavedModChange::Added});
    CHECK(differences[3] == SavedModDifference{mod("m", "Mid", "2"), SavedModChange::OtherVersion});
    CHECK(differences[4] == SavedModDifference{mod("z", "Zed", "2"), SavedModChange::OtherVersion});
}
