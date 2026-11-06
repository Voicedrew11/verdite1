// Reload on death (kf/platform/death_reload.h): what the session last saved or loaded, and the
// load itself, through the same replacement of the game in play as the quick load.
#include "death_reload_rule.h"
#include "internal.hpp"

#include <kf/game/player.h>
#include <kf/game/save.h>
#include <kf/platform/coop.h>
#include <kf/platform/death_reload.h>
#include <kf/platform/host.h>

#include <array>
#include <cstdio>
#include <optional>

namespace kf {
namespace {

struct DeathReloadState {
    std::optional<SaveSlot> last_save;
    SaveSlot loading = SaveSlot::First;   // what load_last_save reads: player_replace_game takes a plain function
};
DeathReloadState state;

bool load_last_save() {
    if (state.loading == SaveSlot::Quick)
        return save_system_quick_read();
    // The load refuses a slot the catalog has not seen as it is now; read it afresh.
    std::array<KfSaveSlotSummary, KF_SAVE_SLOT_COUNT> summaries;
    if (save_system_read_catalog(summaries) != KF_SAVE_RESULT_OK)
        return false;
    return save_system_read_slot(state.loading) == KF_SAVE_RESULT_OK;
}

} // namespace

void death_reload_note_save(SaveSlot slot) {
    if (slot >= SaveSlot::First && slot <= SaveSlot::Quick)
        state.last_save = slot;
}

void death_reload_new_game() {
    state.last_save.reset();
}

bool death_reload_last_save() {
    const ext::DeathReloadFacts facts{ext::effective_settings().reload_on_death, coop_world_in_play(), state.last_save};
    const std::optional<SaveSlot> slot = ext::death_reload_slot(facts);
    if (!slot) {
        if (facts.setting_on && !facts.coop_world)
            std::printf("[reload on death] no save made or loaded yet; the game restarts\n");
        return false;
    }
    state.loading = *slot;
    if (!player_replace_game(load_last_save)) {
        std::printf("[reload on death] slot %d would not load; the game restarts\n", static_cast<int>(*slot));
        host_show_message("That save could not be loaded.");
        return false;
    }
    std::printf("[reload on death] reloaded slot %d\n", static_cast<int>(*slot));
    host_show_message(*slot == SaveSlot::Quick ? "Reloaded the quick save" : "Reloaded the last save");
    return true;
}

} // namespace kf
