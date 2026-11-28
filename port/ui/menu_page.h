#ifndef KF_UI_MENU_PAGE_H
#define KF_UI_MENU_PAGE_H
// A tab of the F1 menu beside the option table's pages: Online, Console,
// About (and Mods). It has rows for the menu model (its buttons, so a pad
// reaches them), and a part of the page written once when the tab is shown,
// where its fields and long text live so a rewrite never takes the typing
// away; each frame it changes only what it must inside that part. Game thread.
#include <verdite/menu_model.h>

#include <memory>
#include <string>
#include <string_view>

namespace verdite::ui {
class Document;
struct Action;
} // namespace verdite::ui

namespace kf::ext {

class MenuPage {
public:
    virtual ~MenuPage() = default;
    [[nodiscard]] virtual std::string title() const = 0;

    // Its rows, as the model asks a tab's (verdite/menu_model.h); none by default.
    [[nodiscard]] virtual int row_count() const;
    [[nodiscard]] virtual verdite::MenuRowShape shape(int row) const;
    virtual void choose(int row, int choice);
    virtual void activate(int row, verdite::ui::Document &page);
    // The rows' RML (menu_row_rml), for the menu's #rows.
    [[nodiscard]] virtual std::string rows_rml(const verdite::MenuModel &model) const;

    // Its part (#part), written when the tab is shown.
    [[nodiscard]] virtual std::string part_rml() const = 0;
    // Each frame while shown: text and classes inside its part.
    virtual void update(verdite::ui::Document &page) = 0;
    // A data-action of its own on the menu's document ("join" on Enter in a field).
    virtual void action(const verdite::ui::Action &action, verdite::ui::Document &page);
};

// A row as the option menu writes one (verdite/option_menu.h's convention):
// row-N, pressed and pointed at by data-action "row".
std::string menu_row_rml(const verdite::MenuModel &model, int index, std::string_view label,
                         std::string_view value_rml, std::string_view kind_class);

std::unique_ptr<MenuPage> make_online_page();
std::unique_ptr<MenuPage> make_console_page();
std::unique_ptr<MenuPage> make_about_page();

} // namespace kf::ext

#endif // KF_UI_MENU_PAGE_H
