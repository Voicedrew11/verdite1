#include "death_reload_rule.h"

namespace kf::ext {

std::optional<SaveSlot> death_reload_slot(const DeathReloadFacts &facts) {
    if (!facts.setting_on || facts.coop_world || !facts.last_save)
        return std::nullopt;
    const SaveSlot slot = *facts.last_save;
    if (slot < SaveSlot::First || slot > SaveSlot::Quick)
        return std::nullopt;
    return slot;
}

} // namespace kf::ext
