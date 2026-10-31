#ifndef KF_PLATFORM_EXTRA_ITEMS_H
#define KF_PLATFORM_EXTRA_ITEMS_H
// PC extension: items the port adds, in item ids the disc leaves unnamed. Each is a row of
// port/ext/extra_items.cpp's table, so a new item (ours or, later, a mod's) is a new row. None is
// kept in item_stock: the Use list offers each while its row says so, and saves and coop's pack are
// unchanged. Using one opens a menu the port fills and the game draws in its own style.
#include <kf/lib/types.h>
#include <kf/platform/language.h>
#include <kf/platform/messages.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace kf {

// The extra items the Use list offers now, in the table's order.
std::vector<u8> extra_items_offered();
// Whether id is an extra item's: its name and model are not the disc's.
bool extra_item_is(u8 id);
// Its name in language, as menu_spell spells it; empty for a disc item.
std::string extra_item_name(u8 id, Language language);
// The disc item whose model the menus show for id: id itself for a disc item.
u8 extra_item_model(u8 id);

// A menu an extra item opens. Closed: back to the Use list. Done: it did what it is for, and the
// item menus close.
enum class ItemMenuKey : u8 { Up, Down, Confirm, Back };
enum class ItemMenuState : u8 { Open, Closed, Done };
// A line is pieces, as a message is read; a piece whose text is item_menu_blank is a blank still to
// fill, drawn as a gap with a line under it (the fonts have no underscore).
using ItemMenuLine = std::vector<MessageTextPiece>;
inline constexpr std::string_view item_menu_blank = "___";
inline constexpr std::size_t item_menu_visible_rows = 7;   // as the title menu's rows
struct ItemMenuPage {
    ItemMenuLine title;
    std::vector<ItemMenuLine> rows;     // the visible rows only
    std::size_t cursor = 0;             // into rows; rows.size() when none is selectable
    bool more_above = false, more_below = false;
    std::vector<ItemMenuLine> footer;   // lines under the rows: a preview, a count
};

// Using an extra item from the Use list. Open when it opened a menu, which the game then drives
// with item_menu_press and draws from item_menu_page until a press returns another state.
ItemMenuState extra_item_use(u8 id);
ItemMenuState item_menu_press(ItemMenuKey key);
ItemMenuPage item_menu_page(Language language);

} // namespace kf

#endif // KF_PLATFORM_EXTRA_ITEMS_H
