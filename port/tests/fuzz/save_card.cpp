// libFuzzer: a save card from outside the game (an emulator's, a C# Verdite build's, an edited
// one), as the game file and as the quick save, read the game's own way: the catalog, each slot,
// the quick load. Whatever it holds, a load must not crash or leave a record pointer outside its
// table.
#include <kf/platform/saves.h>
#include <kf/game/effect.h>
#include <kf/game/equipment.h>
#include <kf/game/player.h>
#include <kf/game/save.h>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

template <typename T, std::size_t N>
bool inside(const T *pointer, const T (&table)[N]) {
    return !pointer || (pointer >= table && pointer < table + N);
}

void check_player() {
    const auto &p = player_state;
    const bool ok = inside(p.equipped_weapon_record, weapon_records.entries) &&
                    inside(p.equipped_head_armor_record, armor_records.entries) &&
                    inside(p.equipped_body_armor_record, armor_records.entries) &&
                    inside(p.equipped_shield_record, armor_records.entries) &&
                    inside(p.equipped_arm_armor_record, armor_records.entries) &&
                    inside(p.equipped_leg_armor_record, armor_records.entries) &&
                    inside(p.selected_magic_record, effect_state.magic.entries);
    if (!ok) {
        std::fprintf(stderr, "a loaded save left a record pointer outside its table\n");
        std::abort();
    }
}

} // namespace

extern "C" int LLVMFuzzerInitialize(int *, char ***) {
    const auto folder = std::filesystem::temp_directory_path() / ("verdite-fuzz-saves-" + std::to_string(::getpid()));
    std::filesystem::create_directories(folder);
    return kf::save_storage_start(folder.c_str()) ? 0 : 1;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    std::array<u8, kf::save_card_bytes> card{};
    std::copy_n(data, std::min(size, card.size()), card.begin());
    if (kf::save_card_write(kf::SaveCard::Game, card) != kf::SaveFileResult::Ok ||
        kf::save_card_write(kf::SaveCard::Quick, card) != kf::SaveFileResult::Ok)
        return 0;
    std::array<KfSaveSlotSummary, KF_SAVE_SLOT_COUNT> summaries{};
    save_system_read_catalog(summaries);
    for (const auto slot : {kf::SaveSlot::First, kf::SaveSlot::Second, kf::SaveSlot::Third}) {
        if (save_system_read_slot(slot) == KF_SAVE_RESULT_OK)
            check_player();
    }
    if (save_system_quick_read())
        check_player();
    return 0;
}
