// The F1 menu (menu.h): KF1's option table on libverdite's option menu.
#include "menu.h"

#include "game_window_art.h"

#include "internal.hpp"
#include "menu_input.h"
#include "menu_page.h"
#include "shell.h"

#include <kf/platform/user_folder.h>

#include <verdite/menu_model.h>
#include <verdite/option_menu.h>
#include <verdite/rml_text.h>
#include <verdite/ui.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace kf::ext {
namespace {
constexpr const char *document_path = "menu.rml";

// KF1's control bindings behind the Controls page's rows (binding_rows.cpp).
class KfBindingSlots final : public verdite::BindingSlots {
public:
    int slot_count(std::string_view row_id) const override {
        const std::optional<BindingRow> row = binding_row(row_id);
        if (!row)
            return 0;
        const std::size_t count = controls(*row).count;
        return static_cast<int>(count < controls_per_action ? count + 1 : count);
    }
    std::string slot_rml(std::string_view row_id, int slot) const override {
        const std::optional<BindingRow> row = binding_row(row_id);
        if (!row)
            return {};
        const ActionControls held = controls(*row);
        if (slot >= 0 && static_cast<std::size_t>(slot) < held.count)
            return verdite::rml_escaped(control_label(held.list[static_cast<std::size_t>(slot)]));
        if (binding_capture_waiting(*row))
            return "<wait>" + std::to_string(binding_capture_seconds_left()) + " s</wait>";
        return "<add>+</add>";
    }
    std::string fixed_rml(std::string_view row_id) const override {
        const std::optional<BindingRow> row = binding_row(row_id);
        if (!row)
            return {};
        std::string out;
        for (const InputBinding &binding : fixed_bindings()) {
            const bool android_back = binding.control == Control{InputDevice::keyboard, SDLK_AC_BACK};
            if (binding.action != row->action || binding_group(binding.control) != row->group || android_back)
                continue;
            out += "<chip>" + verdite::rml_escaped(control_label(binding.control)) + "</chip>";
        }
        return out;
    }
    void listen(std::string_view row_id, int) override {
        if (const std::optional<BindingRow> row = binding_row(row_id))
            if (controls(*row).count < controls_per_action)
                binding_capture_start(*row);
    }
    void clear(std::string_view row_id, int slot) override {
        const std::optional<BindingRow> row = binding_row(row_id);
        if (!row)
            return;
        const ActionControls held = controls(*row);
        if (slot >= 0 && static_cast<std::size_t>(slot) < held.count)
            binding_remove(*row, held.list[static_cast<std::size_t>(slot)]);
    }
    bool listening(std::string_view row_id, int slot) const override {
        const std::optional<BindingRow> row = binding_row(row_id);
        return row && binding_capture_waiting(*row) &&
               static_cast<std::size_t>(slot) == controls(*row).count;
    }

private:
    static ActionControls controls(BindingRow row) {
        return bindings_in_play(row.group)[static_cast<std::size_t>(row.action)];
    }
};

verdite::ui::Document *document() {
    verdite::ui::Screen *screen = interface_screen();
    return screen ? screen->load(document_path) : nullptr;
}

// The option table's pages, then KF1's own (menu_page.h), as one menu's tabs.
class MenuTabs final : public verdite::MenuRows {
public:
    explicit MenuTabs(verdite::OptionMenu &options) : options_(options) {
        pages_.push_back(make_online_page());
        pages_.push_back(make_console_page());
        pages_.push_back(make_about_page());
    }

    // The page a tab shows, or null for the option table's.
    MenuPage *page(int tab) const {
        const int at = tab - options_.tab_count();
        return at >= 0 && at < static_cast<int>(pages_.size()) ? pages_[static_cast<std::size_t>(at)].get() : nullptr;
    }
    std::string tabs_rml(const verdite::MenuModel &model) const {
        std::string out = options_.tabs_rml(model);
        for (std::size_t i = 0; i < pages_.size(); ++i) {
            const int tab = options_.tab_count() + static_cast<int>(i);
            out += "<tab data-action=\"tab\" data-value=\"" + std::to_string(tab) + "\"";
            out += tab == model.tab() ? " class=\"chosen\">" : ">";
            out += verdite::rml_escaped(pages_[i]->title()) + "</tab>";
        }
        return out;
    }

    std::string title(int tab) const {
        const MenuPage *shown = page(tab);
        return shown ? shown->title() : options_.tab_title(tab);
    }

    int tab_count() const override { return options_.tab_count() + static_cast<int>(pages_.size()); }
    int row_count(int tab) const override {
        const MenuPage *shown = page(tab);
        return shown ? shown->row_count() : options_.row_count(tab);
    }
    verdite::MenuRowShape shape(int tab, int row) const override {
        const MenuPage *shown = page(tab);
        return shown ? shown->shape(row) : options_.shape(tab, row);
    }
    void choose(int tab, int row, int choice) override {
        if (MenuPage *shown = page(tab))
            shown->choose(row, choice);
        else
            options_.choose(tab, row, choice);
    }
    void step(int tab, int row, int delta) override {
        if (!page(tab))
            options_.step(tab, row, delta);
    }
    void activate(int tab, int row) override {
        MenuPage *shown = page(tab);
        verdite::ui::Document *doc = document();
        if (shown && doc)
            shown->activate(row, *doc);
        else if (!shown)
            options_.activate(tab, row);
    }
    void listen(int tab, int row, int slot) override {
        if (!page(tab))
            options_.listen(tab, row, slot);
    }
    void clear(int tab, int row, int slot) override {
        if (!page(tab))
            options_.clear(tab, row, slot);
    }
    bool choice_enabled(int tab, int row, int choice) const override {
        return page(tab) ? true : options_.choice_enabled(tab, row, choice);
    }

private:
    verdite::OptionMenu &options_;
    std::vector<std::unique_ptr<MenuPage>> pages_;
};

struct Menu {
    KfBindingSlots bindings;
    verdite::OptionMenu options{kf::ext::options(), platform_features(), &bindings};
    MenuTabs tabs{options};
    verdite::MenuModel model{tabs};
    bool open = false;
    MenuInputs inputs;
    int shown_row = -1;      // the focused row last scrolled into view
    int part_tab = -1;       // the tab whose part #part holds
};
std::unique_ptr<Menu> menu;

Menu &the_menu() {
    if (!menu)
        menu = std::make_unique<Menu>();
    return *menu;
}

void press(verdite::MenuInput input) {
    Menu &m = the_menu();
    if (m.model.press(input) == verdite::MenuResult::Close)
        menu_set_open(false);
    static_cast<void>(m.model.take_cue());   // (KF1 has no menu sounds of its own to play)
}

std::string prompts_rml() {
    return "<prompt><key>&#x2191;&#x2193;</key> choose</prompt><prompt><key>&#x2190;&#x2192;</key> change</prompt>"
           "<prompt><key>Enter</key> confirm</prompt><prompt><key>Del</key> clear</prompt>"
           "<prompt><key>Q</key><key>E</key> page</prompt><prompt><key>Esc</key> back</prompt>";
}

void write_document(verdite::ui::Document &page) {
    Menu &m = the_menu();
    MenuPage *shown = m.tabs.page(m.model.tab());
    if (m.part_tab != m.model.tab()) {   // a page's part is written once, so its fields keep their typing
        m.part_tab = m.model.tab();
        page.set_rml("part", shown ? shown->part_rml() : std::string());
    }
    if (shown)
        shown->update(page);
    page.set_text("heading", m.tabs.title(m.model.tab()));
    page.set_rml("tabs", m.tabs.tabs_rml(m.model));
    page.set_rml("rows", shown ? shown->rows_rml(m.model) : m.options.rows_rml(m.model));
    page.set_rml("help", shown ? std::string() : m.options.help_rml(m.model));
    page.set_classes("help", shown ? "hidden" : "");
    page.set_rml("prompts", prompts_rml());
    page.set_text("version", std::string(product_name) + " " + KF_VERSION);
    page.set_rml("update", update_notice_badge_rml());
    const bool touch = (platform_features() & static_cast<verdite::PlatformFeatures>(
                                                  verdite::PlatformFeature::Touch)) != 0 &&
                       (platform_features() & static_cast<verdite::PlatformFeatures>(
                                                  verdite::PlatformFeature::Mouse)) == 0;
    std::string body = touch ? "touch" : "";
    if (game_window_art().available())
        body += body.empty() ? "game-art" : " game-art";
    page.set_body_classes(body);
}
} // namespace

bool menu_open() {
    return menu && menu->open;
}

void menu_set_open(bool open) {
    Menu &m = the_menu();
    if (m.open == open)
        return;
    verdite::ui::Document *page = document();
    if (open && !page)
        return;   // no interface: nothing to open
    m.open = open;
    m.inputs.reset();
    if (open) {
        SDL_SetWindowRelativeMouseMode(window(), false);
        m.options.refresh();
        m.model.open_at(m.model.tab());
        write_document(*page);
        page->show();
        return;
    }
    binding_capture_cancel();
    if (page)
        page->hide();
}

void menu_open_page(std::string_view title) {
    menu_set_open(true);
    Menu &m = the_menu();
    int tab = m.options.tab_of(title);
    for (int each = m.options.tab_count(); tab < 0 && each < m.tabs.tab_count(); ++each)
        if (m.tabs.page(each)->title() == title)
            tab = each;
    m.model.open_at(tab >= 0 ? tab : 0);
}

bool menu_event(const SDL_Event &event) {
    if (!menu_open())
        return false;
    const bool pad = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP ||
                     event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION;
    if (!pad && event.type != SDL_EVENT_KEY_DOWN && event.type != SDL_EVENT_KEY_UP)
        return false;
    if (pad && binding_capture_busy())
        return true;
    if (const std::optional<verdite::MenuInput> input = the_menu().inputs.take(event))
        press(*input);
    return true;
}

void menu_action(const verdite::ui::Action &action) {
    if (!menu_open() || action.document != document_path)
        return;
    Menu &m = the_menu();
    bool known = false;
    if (action.kind == verdite::ui::ActionKind::Hover)
        known = verdite::menu_hover(m.model, action.name, action.value);
    else if (action.kind == verdite::ui::ActionKind::Press)
        known = verdite::menu_press(m.model, action.name, action.value);
    MenuPage *shown = m.tabs.page(m.model.tab());
    verdite::ui::Document *page = document();
    if (!known && shown && page)
        shown->action(action, *page);
    static_cast<void>(m.model.take_cue());
}

void menu_frame() {
    binding_capture_frame();
    if (!menu_open())
        return;
    verdite::ui::Document *page = document();
    if (!page)
        return;
    Menu &m = the_menu();
    if (const std::optional<verdite::MenuInput> input = m.inputs.repeat())
        press(*input);
    m.options.refresh();
    m.model.rows_changed();
    write_document(*page);
    if (m.model.row() != m.shown_row) {
        m.shown_row = m.model.row();
        page->scroll_into_view("row-" + std::to_string(m.shown_row));
    }
}
} // namespace kf::ext
