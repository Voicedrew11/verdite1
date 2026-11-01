#include "mod_rule_values.h"

namespace kf::ext {

void RuleValues::set(const RuleKey &key, const RuleFields &fields) {
    if (fields.empty())
        return;
    RuleFields &record = records_[key];
    for (const auto &[field, value] : fields)
        record[field] = value;
}

const RuleFields *RuleValues::find(const RuleKey &key) const {
    const auto found = records_.find(key);
    return found == records_.end() ? nullptr : &found->second;
}

RuleValues combine_rule_values(std::span<const RuleValues *const> in_load_order) {
    RuleValues combined;
    for (const RuleValues *mod : in_load_order)
        for (const auto &[key, fields] : mod->records())
            combined.set(key, fields);
    return combined;
}

} // namespace kf::ext
