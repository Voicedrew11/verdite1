#pragma once
// The port's side of kf/platform/extra_items.h: what a row of extra_items.cpp's table is, and the
// menu an item opens. A new extra item is a new row and, if it opens a menu, a new ItemMenu.
#include <kf/platform/extra_items.h>

#include <memory>
#include <string_view>

namespace kf::ext {

class ItemMenu {
public:
    virtual ~ItemMenu() = default;
    [[nodiscard]] virtual ItemMenuState press(ItemMenuKey key) = 0;
    [[nodiscard]] virtual ItemMenuPage page(Language language) const = 0;
};

struct ExtraItem {
    u8 id = 0;                              // an id below KF_ITEM_COUNT that the disc leaves unnamed
    std::string_view english, japanese;     // its name: capitals, and full-size kana
    std::string_view description;           // English, for the item menus' description panel
    u8 model_item = 0;                      // the disc item whose model stands for it
    bool (*offered)() = nullptr;            // in the Use list now
    std::unique_ptr<ItemMenu> (*open)() = nullptr;   // the menu using it opens; none acts at once
};

// The item's row, or none for a disc item. Its description, for item_text.cpp.
const ExtraItem *extra_item(u8 id);

} // namespace kf::ext
