// Keys, pad buttons and the stick as menu inputs (menu_input.h)
#include "menu_input.h"

#include "internal.hpp"

namespace kf::ext {
namespace {
constexpr Uint64 repeat_first_ms = 350;   // a held direction moves again after this
constexpr Uint64 repeat_next_ms = 85;     // then this often
constexpr Sint16 stick_push = 16000;      // the left stick past this is a direction

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
} // namespace

std::optional<verdite::MenuInput> MenuInputs::take(const SDL_Event &event) {
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
        return key_input(event.key);
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        const std::optional<verdite::MenuInput> input = pad_input(event.gbutton.button);
        if (input && is_direction(*input))
            return hold(*input);
        return input;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (const std::optional<verdite::MenuInput> input = pad_input(event.gbutton.button))
            let_go(*input);
        return std::nullopt;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        return stick(event.gaxis);
    default:
        return std::nullopt;
    }
}

std::optional<verdite::MenuInput> MenuInputs::repeat() {
    const Uint64 now_ms = SDL_GetTicks();
    if (!held_ || now_ms < held_next_ms_)
        return std::nullopt;
    held_next_ms_ = now_ms + repeat_next_ms;
    return held_;
}

void MenuInputs::reset() {
    held_.reset();
    stick_x_.reset();
    stick_y_.reset();
}

std::optional<verdite::MenuInput> MenuInputs::hold(verdite::MenuInput input) {
    held_ = input;
    held_next_ms_ = SDL_GetTicks() + repeat_first_ms;
    return input;
}

void MenuInputs::let_go(verdite::MenuInput input) {
    if (held_ == input)
        held_.reset();
}

// The left stick as the d-pad: a direction when pushed past stick_push.
std::optional<verdite::MenuInput> MenuInputs::stick(const SDL_GamepadAxisEvent &axis) {
    using verdite::MenuInput;
    const bool across = axis.axis == SDL_GAMEPAD_AXIS_LEFTX;
    if (!across && axis.axis != SDL_GAMEPAD_AXIS_LEFTY)
        return std::nullopt;
    std::optional<MenuInput> &was = across ? stick_x_ : stick_y_;
    std::optional<MenuInput> now;
    if (axis.value > stick_push)
        now = across ? MenuInput::Right : MenuInput::Down;
    else if (axis.value < -stick_push)
        now = across ? MenuInput::Left : MenuInput::Up;
    if (now == was)
        return std::nullopt;
    if (was)
        let_go(*was);
    was = now;
    return now ? hold(*now) : std::nullopt;
}

} // namespace kf::ext
