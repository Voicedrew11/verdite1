// The F1 menu (menu.h): KF1's option table on libverdite's option menu.
#include "menu.h"

#include "internal.hpp"
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
constexpr Uint64 repeat_first_ms = 350;   // a held direction moves again after this
constexpr Uint64 repeat_next_ms = 85;     // then this often
constexpr Sint16 stick_push = 16000;      // the left stick past this is a direction

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
    std::optional<verdite::MenuInput> held;   // a direction held on the pad or the stick
    Uint64 held_next_ms = 0;
    verdite::MenuInput stick_x_held = verdite::MenuInput::Clear, stick_y_held = verdite::MenuInput::Clear;
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

std::optional<verdite::MenuInput> key_input(const SDL_KeyboardEvent &key) {
    using verdite::MenuInput;
    const bool shift = (key.mod & SDL_KMOD_SHIFT) != 0;
    switch (key.key) {
    case SDLK_UP: case SDLK_W: return MenuInput::Up;
    case SDLK_DOWN: case SDLK_S: return MenuInput::Down;
    case SDLK_LEFT: case SDLK_A: return MenuInput::Left;
    case SDLK_RIGHT: case SDLK_D: return MenuInput::Right;
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: return MenuInput::Confirm;
    case SDLK_ESCAPE: case SDLK_BACKSPACE: return MenuInput::Back;
    case SDLK_DELETE: return MenuInput::Clear;
    case SDLK_E: case SDLK_PAGEDOWN: return MenuInput::NextTab;
    case SDLK_Q: case SDLK_PAGEUP: return MenuInput::PreviousTab;
    case SDLK_TAB: return shift ? MenuInput::PreviousTab : MenuInput::NextTab;
    default: return std::nullopt;
    }
}

// The pad's buttons, with confirm and back where the player's layout has them.
std::optional<verdite::MenuInput> pad_input(Uint8 button) {
    using verdite::MenuInput;
    const bool japanese = pad_layout_of(settings().pad_layout) == PadLayout::Japanese;
    const Uint8 confirm = japanese ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH;
    const Uint8 back = japanese ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST;
    if (button == confirm)
        return MenuInput::Confirm;
    if (button == back)
        return MenuInput::Back;
    switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return MenuInput::Up;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return MenuInput::Down;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return MenuInput::Left;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return MenuInput::Right;
    case SDL_GAMEPAD_BUTTON_WEST: return MenuInput::Clear;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return MenuInput::PreviousTab;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return MenuInput::NextTab;
    case SDL_GAMEPAD_BUTTON_START: return MenuInput::Back;
    default: return std::nullopt;
    }
}

bool is_direction(verdite::MenuInput input) {
    using verdite::MenuInput;
    return input == MenuInput::Up || input == MenuInput::Down || input == MenuInput::Left ||
           input == MenuInput::Right;
}

void press(verdite::MenuInput input) {
    Menu &m = the_menu();
    if (m.model.press(input) == verdite::MenuResult::Close)
        menu_set_open(false);
    static_cast<void>(m.model.take_cue());   // (KF1 has no menu sounds of its own to play)
}

void hold(verdite::MenuInput input) {
    Menu &m = the_menu();
    m.held = input;
    m.held_next_ms = SDL_GetTicks() + repeat_first_ms;
    press(input);
}

void let_go(verdite::MenuInput input) {
    Menu &m = the_menu();
    if (m.held == input)
        m.held.reset();
}

// The left stick as the d-pad: a direction when pushed past stick_push.
bool stick_event(const SDL_GamepadAxisEvent &axis) {
    using verdite::MenuInput;
    Menu &m = the_menu();
    const bool across = axis.axis == SDL_GAMEPAD_AXIS_LEFTX;
    if (!across && axis.axis != SDL_GAMEPAD_AXIS_LEFTY)
        return true;
    MenuInput &was = across ? m.stick_x_held : m.stick_y_held;
    MenuInput now = MenuInput::Clear;   // (Clear: none)
    if (axis.value > stick_push)
        now = across ? MenuInput::Right : MenuInput::Down;
    else if (axis.value < -stick_push)
        now = across ? MenuInput::Left : MenuInput::Up;
    if (now == was)
        return true;
    if (was != MenuInput::Clear)
        let_go(was);
    was = now;
    if (now != MenuInput::Clear)
        hold(now);
    return true;
}

void repeat_held() {
    Menu &m = the_menu();
    if (!m.held || SDL_GetTicks() < m.held_next_ms)
        return;
    m.held_next_ms = SDL_GetTicks() + repeat_next_ms;
    press(*m.held);
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
    m.held.reset();
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
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
        if (std::optional<verdite::MenuInput> input = key_input(event.key))
            press(*input);   // (a key's own repeat is its repeat)
        return true;
    case SDL_EVENT_KEY_UP:
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (binding_capture_busy())
            return true;
        if (std::optional<verdite::MenuInput> input = pad_input(event.gbutton.button))
            is_direction(*input) ? hold(*input) : press(*input);
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (std::optional<verdite::MenuInput> input = pad_input(event.gbutton.button))
            let_go(*input);
        return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        return binding_capture_busy() || stick_event(event.gaxis);
    default:
        return false;
    }
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
    repeat_held();
    Menu &m = the_menu();
    m.rows.refresh();
    m.model.rows_changed();
    write_document(*page);
    if (m.model.row() != m.shown_row) {
        m.shown_row = m.model.row();
        page->scroll_into_view("row-" + std::to_string(m.shown_row));
    }
}
} // namespace kf::ext
