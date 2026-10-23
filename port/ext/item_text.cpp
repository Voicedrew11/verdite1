// What each item does, for the item menus and the map's hover labels. Effects
// only: the menus already show equipment stats. Checked against the game's
// own rules (player_use_item, menu_use_item_panel, the COM.DAT equipment
// records, which tick at 20 per second) and the Sword of Moonlight wiki.
#include "internal.hpp"
#include <imgui.h>
#include <kf/platform/host.h>
#include <algorithm>
#include <cfloat>

namespace kf::ext {
namespace {
constexpr const char *sword_magic = "Sword magic: press Magic during a fully charged swing";

const char *const descriptions[] = {
    /* 0 Short Sword */ nullptr,
    /* 1 Battle Axe */ nullptr,
    /* 2 Knight Sword */ nullptr,
    /* 3 Colichemarde */ nullptr,   // see item_description
    /* 4 Morning Star */ nullptr,
    /* 5 Bastard Sword */ nullptr,
    /* 6 Crescent Axe */ nullptr,
    /* 7 Triple Fang */ nullptr,
    /* 8 Flame Sword */ nullptr,
    /* 9 Shadow Blade */ "Cursed: darkens your vision while held.",
    /* 10 Dragon Sword */ "Restores 1 MP every 2 seconds.",
    /* 11 Moonlight Sword */ nullptr,
    nullptr,
    /* 13 Iron Mask */ nullptr,
    /* 14 Knight Helm */ nullptr,
    /* 15 Great Helm */ nullptr,
    /* 16 Blood Mask */ "Cursed: drains 1 HP every 1.5 seconds.",
    /* 17 Mist Crown */ "Restores 1 HP every 0.75 seconds.",
    nullptr,
    /* 19 Breast Plate */ nullptr,
    /* 20 Knight Plate */ nullptr,
    /* 21 Full Plate */ "Covers the arms and legs too: no other arm or leg armor can be worn.",
    /* 22 Fire Mail */ nullptr,
    /* 23 Skull Armor */ "Cursed: no magic can be cast while it is worn.",
    /* 24 Forest Armor */ nullptr,
    nullptr,
    /* 26 Small Shield */ nullptr,
    /* 27 Knight Shield */ nullptr,
    /* 28 Tower Shield */ nullptr,
    /* 29 Black Dragon */ nullptr,
    /* 30 Wind Guard */ nullptr,
    nullptr,
    /* 32 Gauntlets */ nullptr,
    /* 33 Knight Gloves */ nullptr,
    /* 34 Stone Hands */ nullptr,
    /* 35 Iron Boots */ nullptr,
    /* 36 Knight Boots */ nullptr,
    /* 37 Leg Guards */ nullptr,
    /* 38 Feather Boots */ "Lets you cross small bottomless pits.",
    /* 39 Gold Coin */ nullptr,
    /* 40 Blood Stone */ "A gem. Sell it to a merchant.",
    /* 41 Moon Stone */ "A gem. Sell it to a merchant.",
    /* 42 Verdite */ "Raises Magic Power by 1.",
    /* 43 Medicinal Herb */ "Heals 25 HP.",
    /* 44 Antidote Herb */ "Heals 10 HP. Cures poison.",
    /* 45 Recovery Medicine */ "Heals 80 HP. Cures poison and slowness.",
    /* 46 Dragon Tree Leaf */ "Heals 150 HP. Cures all ailments.",
    /* 47 Dragon Tree Fruit */ "Heals 300 HP, fully restores MP and cures all ailments. Once the Dragon "
                               "Fountain flows again, one revives you there on death.",
    /* 48 Light Ring */ nullptr,
    /* 49 Moon Charm */ "Prevents darkness (blindness).",
    /* 50 Wind Blade Bracelet */ "Teaches Wind Cutter. Wind Cutter costs half the MP while it is worn.",
    /* 51 Twin Dragon Ring */ nullptr,
    /* 52 Gold Cross */ "Juedds Kross will trade a key for it.",
    /* 53 Deceased's Key */ "Opens doors and chests locked with this key.",
    /* 54 Light Family Key */ "Opens doors and chests locked with this key.",
    /* 55 Keeper's Map */ "Shows the map of the current floor. Reveals the whole floor on the automap.",
    /* 56 Dragon Cup */ "Return it to the Dragon Temple to make the Dragon Fountain flow again.",
    /* 57 Phantom Rod */ "Reveals hidden walls for about 50 seconds.",
    /* 58 Green Dragon Rod */ "Warps you back to the entrance of the current floor.",
    /* 59 Truth Mirror */ "Use it facing a creature or person to learn about them.",
    /* 60 Dungeon Key */ "Opens doors and chests locked with this key.",
    /* 61 */ nullptr,
    /* 62 Harp */ "Played at the right ledges, it raises hidden bridges for a short time.",
    /* 63 Water Seal Stone */ "Set it on its pedestal in the Tower of Prayer. All four seal stones open the way.",
    /* 64 Earth Seal Stone */ "Set it on its pedestal in the Tower of Prayer. All four seal stones open the way.",
    nullptr, nullptr, /* 67 */ nullptr,
    /* 68 Fire Seal Stone */ "Set it on its pedestal in the Tower of Prayer. All four seal stones open the way.",
    /* 69 Wind Seal Stone */ "Set it on its pedestal in the Tower of Prayer. All four seal stones open the way.",
    /* 70 */ nullptr, nullptr, nullptr,
    /* 73 Magician's Map */ "Shows a detailed map of the current floor. Reveals the whole floor on the automap.",
    /* 74 Magician's Key */ "Opens doors and chests locked with this key.",
    /* 75 */ nullptr, /* 76 */ nullptr, nullptr, nullptr, nullptr};
static_assert(sizeof descriptions / sizeof descriptions[0] == 80, "one entry per item id");
}

std::string item_description(std::uint8_t id) {
    // Weapon magic: the requirements are the game's checks in player_update_weapon_magic.
    switch (id) {
    case 3:
        return std::string(sword_magic) + " to fire light needles (needs 60 Strength and 60 Magic).";
    case 7:
        return "Restores 1 HP and 1 MP every 3 seconds. " + std::string(sword_magic)
            + " to fire a homing bolt (needs 80 Strength and 80 Magic).";
    case 8:
        return std::string(sword_magic) + " to fire fireballs (needs the Fire Ball spell).";
    case 11:
        return "Restores 1 MP every second. " + std::string(sword_magic)
            + " to fire a crescent beam (needs 80 Strength and 80 Magic).";
    default:
        break;
    }
    if (id < sizeof descriptions / sizeof descriptions[0] && descriptions[id])
        return descriptions[id];
    return {};
}

void item_description_draw() {
    // The preview panel's inside in the original 320x240 menu layout, and the
    // text size in those units.
    constexpr float panel_left = 174, panel_right = 300, panel_bottom = 216;
    constexpr float text_units = 8.5f, pad_units = 2.5f;
    const MenuPreviewItem preview = host_menu_preview();
    if (preview.item < 0 || !effective_settings().item_descriptions)
        return;
    const std::string text = item_description(static_cast<std::uint8_t>(preview.item));
    if (text.empty())
        return;
    const ViewRect view = original_view();
    const float sx = view.w / 320.0f, sy = view.h / 240.0f;
    ImFont *font = ImGui::GetFont();
    const float size = text_units * sy, pad = pad_units * sy;
    const float wrap = (panel_right - panel_left) * sx - 2 * pad;
    const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, wrap, text.c_str());
    // Bottom of the panel, growing upwards over the model but never over the stats.
    const float top = std::max(view.y + (static_cast<float>(preview.top) + 3) * sy,
                               view.y + panel_bottom * sy - extent.y - 2 * pad);
    const ImVec2 from(view.x + panel_left * sx, top);
    ImDrawList *draw = ImGui::GetBackgroundDrawList();
    draw->AddRectFilled(from, ImVec2(view.x + panel_right * sx, top + extent.y + 2 * pad), IM_COL32(24, 26, 52, 185),
                        pad);
    draw->AddText(font, size, ImVec2(from.x + pad, from.y + pad), IM_COL32(232, 226, 204, 255), text.c_str(), nullptr,
                  wrap);
}
}
