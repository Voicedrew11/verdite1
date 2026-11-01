#include "../ext/mod_rule_values.h"

#include <doctest/doctest.h>

#include <array>

using namespace kf;
using namespace kf::ext;

namespace {
const RuleKey monster{RulesTable::ActorDefinitions, 1, 3};
const RuleKey other_floor{RulesTable::ActorDefinitions, 2, 3};
const RuleKey price{RulesTable::ShopPrices, 43, 1};

std::int64_t field_of(const RuleValues &values, const RuleKey &key, std::size_t field) {
    const RuleFields *fields = values.find(key);
    REQUIRE(fields != nullptr);
    REQUIRE(fields->contains(field));
    return fields->at(field);
}
} // namespace

TEST_CASE("no mods leave nothing over the originals") {
    CHECK(combine_rule_values({}).records().empty());
}

TEST_CASE("a second call merges its fields over the mod's earlier ones") {
    RuleValues mod;
    mod.set(monster, {{0, 100}, {1, 20}});
    mod.set(monster, {{1, 30}, {2, 5}});
    CHECK(field_of(mod, monster, 0) == 100);
    CHECK(field_of(mod, monster, 1) == 30);
    CHECK(field_of(mod, monster, 2) == 5);
}

TEST_CASE("an empty call sets no record") {
    RuleValues mod;
    mod.set(monster, {});
    CHECK(mod.find(monster) == nullptr);
}

TEST_CASE("records are kept apart by table and place") {
    RuleValues mod;
    mod.set(monster, {{0, 100}});
    mod.set(other_floor, {{0, 200}});
    mod.set(price, {{0, 7}});
    CHECK(field_of(mod, monster, 0) == 100);
    CHECK(field_of(mod, other_floor, 0) == 200);
    CHECK(field_of(mod, price, 0) == 7);
    CHECK(mod.records().size() == 3);
}

TEST_CASE("a later mod wins per field, and the earlier mod's other fields stay") {
    RuleValues first;
    RuleValues second;
    first.set(monster, {{0, 100}, {1, 20}});
    second.set(monster, {{0, 300}});
    second.set(price, {{1, 9}});
    const std::array<const RuleValues *, 2> in_order{&first, &second};
    const RuleValues combined = combine_rule_values(in_order);
    CHECK(field_of(combined, monster, 0) == 300);
    CHECK(field_of(combined, monster, 1) == 20);
    CHECK(field_of(combined, price, 1) == 9);

    const std::array<const RuleValues *, 2> reversed{&second, &first};
    CHECK(field_of(combine_rule_values(reversed), monster, 0) == 100);
}

TEST_CASE("a mod left out of the combination takes its values with it") {
    RuleValues first;
    RuleValues second;
    first.set(monster, {{0, 100}});
    second.set(price, {{0, 5}});
    const std::array<const RuleValues *, 1> only_second{&second};
    const RuleValues combined = combine_rule_values(only_second);
    CHECK(combined.find(monster) == nullptr);
    CHECK(field_of(combined, price, 0) == 5);
}
