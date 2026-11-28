// What a page of the F1 menu does by default (menu_page.h)
#include "menu_page.h"

#include <verdite/rml_text.h>

namespace kf::ext {

int MenuPage::row_count() const {
    return 0;
}

verdite::MenuRowShape MenuPage::shape(int) const {
    return {};
}

void MenuPage::choose(int, int) {}

void MenuPage::activate(int, verdite::ui::Document &) {}

std::string MenuPage::rows_rml(const verdite::MenuModel &) const {
    return {};
}

void MenuPage::action(const verdite::ui::Action &, verdite::ui::Document &) {}

std::string menu_row_rml(const verdite::MenuModel &model, int index, std::string_view label,
                         std::string_view value_rml, std::string_view kind_class) {
    const std::string here = std::to_string(index);
    std::string classes(kind_class);
    if (model.row() == index)
        classes += " focused";
    return "<row id=\"row-" + here + "\" data-action=\"row\" data-hover=\"row\" data-value=\"" + here +
           "\" class=\"" + classes + "\"><label>" + verdite::rml_escaped(label) + "</label>" +
           std::string(value_rml) + "</row>";
}

} // namespace kf::ext
