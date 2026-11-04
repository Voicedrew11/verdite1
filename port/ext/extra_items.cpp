// The items the port adds to the Use list (kf/platform/extra_items.h, docs/MESSAGES_PLAN.md). The
// table is the whole list: a new item is a row, and a row that opens a menu names the function
// that makes it. Ids are below 80 and unused by the disc; each row's id is unique.
#include "internal.hpp"
#include "item_menu.h"
#include "mod_content.h"

#include <kf/lib/item_types.h>
#include <kf/platform/mod_events.h>

#include <array>
#include <memory>
#include <string>

namespace kf::ext {
namespace {

constexpr u8 KF_ITEM_MOONSTONE_MODEL_FALLBACK = 41;

// Each id is below 80 (the disc's item ids) and unused by the disc: 79 names no item there. Keep
// the ids unique.
constexpr std::array<ExtraItem, 1> extra_item_table{{
    {.id = 79,
     .english = "MESSAGE STONE",
     .japanese = "ことばの いし",
     .description = "Leave a message on the ground for other players, or see how yours were rated. "
                    "Offered in a coop room, and while Settings, Interface, Online, Messages is on.",
     .model_item = 41,   // the Moonstone's model
     .offered = messages_stone_offered,
     .open = messages_stone_open},
}};

// The menu an extra item opened, if one is open. Game thread only.
struct OpenMenu {
    std::unique_ptr<ItemMenu> menu;
};
OpenMenu open_menu;

} // namespace

const ExtraItem *extra_item(u8 id) {
    for (const ExtraItem &row : extra_item_table)
        if (row.id == id)
            return &row;
    return id >= KF_ITEM_MOD_FIRST ? mod_content_item(id) : nullptr;
}

} // namespace kf::ext

namespace kf {

std::vector<u8> extra_items_offered() {
    std::vector<u8> ids;
    for (const ext::ExtraItem &row : ext::extra_item_table)
        if (row.offered && row.offered())
            ids.push_back(row.id);
    return ids;
}

std::vector<u8> extra_items_stocked(std::span<const u8> stock) {
    std::vector<u8> ids;
    for (std::size_t id = KF_ITEM_MOD_FIRST; id < KF_ITEM_ID_COUNT && id < stock.size(); ++id)
        if (stock[id] != 0 && extra_item_named(static_cast<s32>(id)))
            ids.push_back(static_cast<u8>(id));
    return ids;
}

bool extra_item_named(s32 id) {
    return id >= KF_ITEM_MOD_FIRST && id < KF_ITEM_ID_COUNT && ext::mod_content_item(static_cast<u8>(id)) != nullptr;
}

bool extra_item_is(u8 id) {
    return (id >= KF_ITEM_MOD_FIRST && id < KF_ITEM_ID_COUNT) || ext::extra_item(id) != nullptr;
}

std::string extra_item_name(u8 id, Language language) {
    const ext::ExtraItem *row = ext::extra_item(id);
    if (!row)
        return {};
    return std::string(language == Language::English ? row->english : row->japanese);
}

u8 extra_item_model(u8 id) {
    const ext::ExtraItem *row = ext::extra_item(id);
    if (row)
        return row->model_item;
    // A mod's item whose mod is gone still lies on floors and shows as something.
    return id >= KF_ITEM_MOD_FIRST && id < KF_ITEM_ID_COUNT ? ext::KF_ITEM_MOONSTONE_MODEL_FALLBACK : id;
}

ItemMenuState extra_item_use(u8 id) {
    if (id >= KF_ITEM_MOD_FIRST && id < KF_ITEM_ID_COUNT)
        return mods_item_used(id) ? ItemMenuState::Done : ItemMenuState::Closed;
    const ext::ExtraItem *row = ext::extra_item(id);
    if (!row || !row->open)
        return ItemMenuState::Done;
    ext::open_menu.menu = row->open();
    return ext::open_menu.menu ? ItemMenuState::Open : ItemMenuState::Closed;
}

ItemMenuState item_menu_press(ItemMenuKey key) {
    if (!ext::open_menu.menu)
        return ItemMenuState::Closed;
    const ItemMenuState state = ext::open_menu.menu->press(key);
    if (state != ItemMenuState::Open)
        ext::open_menu.menu.reset();
    return state;
}

ItemMenuPage item_menu_page(Language language) {
    if (!ext::open_menu.menu)
        return {};
    return ext::open_menu.menu->page(language);
}

} // namespace kf
