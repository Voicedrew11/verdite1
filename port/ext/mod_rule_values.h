#pragma once
// The values each mod gives kf.rules, and how they lie over each other: per field, a later mod in
// load order wins. Plain data: mod_rules.cpp lays the result over the game's tables.
#include <kf/platform/mod_events.h>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>

namespace kf::ext {

// A record a rule changes: its table, and its place there (floor and kind, level, item and shop
// bank, item; second is 0 where one number names it).
struct RuleKey {
    RulesTable table = RulesTable::ActorDefinitions;
    int first = 0;
    int second = 0;
    auto operator<=>(const RuleKey &) const = default;
};

// The fields set on a record, by the field's index in its table's field list.
using RuleFields = std::map<std::size_t, std::int64_t>;

class RuleValues {
  public:
    // Sets these fields of the record over those set before; the others stay.
    void set(const RuleKey &key, const RuleFields &fields);
    // The fields set on a record, or nullptr when none are.
    [[nodiscard]] const RuleFields *find(const RuleKey &key) const;
    [[nodiscard]] const std::map<RuleKey, RuleFields> &records() const {
        return records_;
    }
    bool operator==(const RuleValues &) const = default;

  private:
    std::map<RuleKey, RuleFields> records_;
};

// Each mod's values in load order, combined: per field, the last mod that sets it wins.
[[nodiscard]] RuleValues combine_rule_values(std::span<const RuleValues *const> in_load_order);

} // namespace kf::ext
