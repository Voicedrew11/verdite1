// The F1 menu (menu.h): KF1's option table on libverdite's option menu.
#include "menu.h"

#include "internal.hpp"
#include "menu_input.h"
#include "shell.h"

#include <kf/platform/user_folder.h>

#include <verdite/menu_model.h>
#include <verdite/option_menu.h>
#include <verdite/rml_text.h>
#include <verdite/ui.h>

#include <memory>
#include <optional>
#include <string>

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

struct Menu {
    KfBindingSlots bindings;
    verdite::OptionMenu rows{options(), platform_features(), &bindings};
    verdite::MenuModel model{rows};
    bool open = false;
    MenuInputs inputs;
    int shown_row = -1;   // the focused row last scrolled into view
};
std::unique_ptr<Menu> menu;

Menu &the_menu() {
    if (!menu)
        menu = std::make_unique<Menu>();
    return *menu;
}

verdite::ui::Document *document() {
    verdite::ui::Screen *screen = interface_screen();
    return screen ? screen->load(document_path) : nullptr;
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
    page.set_rml("tabs", m.rows.tabs_rml(m.model));
    page.set_rml("rows", m.rows.rows_rml(m.model));
    page.set_rml("help", m.rows.help_rml(m.model));
    page.set_rml("prompts", prompts_rml());
    page.set_text("version", std::string(product_name) + " " + KF_VERSION);
    const bool touch = (platform_features() & static_cast<verdite::PlatformFeatures>(
                                                  verdite::PlatformFeature::Touch)) != 0 &&
                       (platform_features() & static_cast<verdite::PlatformFeatures>(
                                                  verdite::PlatformFeature::Mouse)) == 0;
    page.set_body_classes(touch ? "touch" : "");
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
        m.rows.refresh();
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
    const int tab = m.rows.tab_of(title);
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
    if (action.kind == verdite::ui::ActionKind::Hover)
        m.rows.hover(m.model, action.name, action.value);
    else if (action.kind == verdite::ui::ActionKind::Press)
        m.rows.press(m.model, action.name, action.value);
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
    m.rows.refresh();
    m.model.rows_changed();
    write_document(*page);
    if (m.model.row() != m.shown_row) {
        m.shown_row = m.model.row();
        page->scroll_into_view("row-" + std::to_string(m.shown_row));
    }
}
} // namespace kf::ext
