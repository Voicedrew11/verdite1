#include <kf/platform/bindings.h>

#include <SDL3/SDL.h>

#include <charconv>

namespace kf {
namespace {
constexpr std::size_t name_bytes_max = 32;
constexpr std::size_t text_bytes_max = 256;

// Keys whose character is blank, a control code or the list separator.
struct KeyName {
    u32 code;
    std::string_view name;
};
constexpr std::array<KeyName, 7> own_key_names{{
    {SDLK_SPACE, "Space"},
    {SDLK_RETURN, "Return"},
    {SDLK_TAB, "Tab"},
    {SDLK_BACKSPACE, "Backspace"},
    {SDLK_ESCAPE, "Escape"},
    {SDLK_DELETE, "Delete"},
    {SDLK_COMMA, "Comma"},
}};

struct ControlName {
    u32 code;
    std::string_view name;
    std::string_view label;
};
constexpr std::array<ControlName, 5> mouse_names{{
    {SDL_BUTTON_LEFT, "MouseLeft", "Left mouse"},
    {SDL_BUTTON_MIDDLE, "MouseMiddle", "Middle mouse"},
    {SDL_BUTTON_RIGHT, "MouseRight", "Right mouse"},
    {SDL_BUTTON_X1, "MouseX1", "Mouse 4"},
    {SDL_BUTTON_X2, "MouseX2", "Mouse 5"},
}};

constexpr u32 button(SDL_GamepadButton button) {
    return static_cast<u32>(button);
}
// By position, with PlayStation names; Guide, the right stick and the triggers' negative ends
// have none. The left stick's are named for labels and tests; it is fixed.
constexpr std::array<ControlName, 31> pad_names{{
    {button(SDL_GAMEPAD_BUTTON_SOUTH), "Cross", "Cross"},
    {button(SDL_GAMEPAD_BUTTON_EAST), "Circle", "Circle"},
    {button(SDL_GAMEPAD_BUTTON_WEST), "Square", "Square"},
    {button(SDL_GAMEPAD_BUTTON_NORTH), "Triangle", "Triangle"},
    {button(SDL_GAMEPAD_BUTTON_BACK), "Select", "Select"},
    {button(SDL_GAMEPAD_BUTTON_START), "Start", "Start"},
    {button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), "L1", "L1"},
    {button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER), "R1", "R1"},
    {pad_axis_positive(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), "L2", "L2"},
    {pad_axis_positive(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), "R2", "R2"},
    {button(SDL_GAMEPAD_BUTTON_LEFT_STICK), "L3", "L3"},
    {button(SDL_GAMEPAD_BUTTON_RIGHT_STICK), "R3", "R3"},
    {button(SDL_GAMEPAD_BUTTON_DPAD_UP), "DpadUp", "D-pad up"},
    {button(SDL_GAMEPAD_BUTTON_DPAD_DOWN), "DpadDown", "D-pad down"},
    {button(SDL_GAMEPAD_BUTTON_DPAD_LEFT), "DpadLeft", "D-pad left"},
    {button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT), "DpadRight", "D-pad right"},
    {pad_axis_negative(SDL_GAMEPAD_AXIS_LEFTY), "LeftStickUp", "Left stick up"},
    {pad_axis_positive(SDL_GAMEPAD_AXIS_LEFTY), "LeftStickDown", "Left stick down"},
    {pad_axis_negative(SDL_GAMEPAD_AXIS_LEFTX), "LeftStickLeft", "Left stick left"},
    {pad_axis_positive(SDL_GAMEPAD_AXIS_LEFTX), "LeftStickRight", "Left stick right"},
    {button(SDL_GAMEPAD_BUTTON_TOUCHPAD), "Touchpad", "Touchpad"},
    {button(SDL_GAMEPAD_BUTTON_LEFT_PADDLE1), "LeftPaddle1", "Left paddle 1"},
    {button(SDL_GAMEPAD_BUTTON_LEFT_PADDLE2), "LeftPaddle2", "Left paddle 2"},
    {button(SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1), "RightPaddle1", "Right paddle 1"},
    {button(SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2), "RightPaddle2", "Right paddle 2"},
    {button(SDL_GAMEPAD_BUTTON_MISC1), "Misc1", "Misc 1"},
    {button(SDL_GAMEPAD_BUTTON_MISC2), "Misc2", "Misc 2"},
    {button(SDL_GAMEPAD_BUTTON_MISC3), "Misc3", "Misc 3"},
    {button(SDL_GAMEPAD_BUTTON_MISC4), "Misc4", "Misc 4"},
    {button(SDL_GAMEPAD_BUTTON_MISC5), "Misc5", "Misc 5"},
    {button(SDL_GAMEPAD_BUTTON_MISC6), "Misc6", "Misc 6"},
}};

// A name as it is matched: ASCII lowercase, without spaces.
std::string folded(std::string_view name) {
    std::string word;
    word.reserve(name.size());
    for (const char c : name) {
        if (c == ' ')
            continue;
        word += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return word;
}

const ControlName *find_code(std::span<const ControlName> table, u32 code) {
    for (const auto &entry : table)
        if (entry.code == code)
            return &entry;
    return nullptr;
}
const ControlName *find_word(std::span<const ControlName> table, std::string_view word) {
    for (const auto &entry : table)
        if (folded(entry.name) == word)
            return &entry;
    return nullptr;
}

// SDL's static name for a scancode as one word: no spaces, "," as "Comma"; empty when unnamed.
// SDL_GetKeyName is not used: it goes through the current keymap.
std::string scancode_word(u32 scancode) {
    if (scancode >= SDL_SCANCODE_COUNT)
        return {};
    std::string word;
    for (const char *c = SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode)); c && *c; ++c) {
        if (*c == ',')
            word += "Comma";
        else if (*c != ' ')
            word += *c;
    }
    return word;
}

bool printable(u32 code_point) {
    const bool surrogate = code_point >= 0xD800 && code_point <= 0xDFFF;
    return code_point >= 0x21 && code_point != 0x7F && code_point <= 0x10FFFF && !surrogate;
}

std::string keycode_name(u32 code) {
    return "Keycode" + std::to_string(code);
}

// SDL's scancode name or the character, which key_name checks reads back as this key.
std::string key_spelling(u32 code) {
    if ((code & SDLK_SCANCODE_MASK) != 0) {
        auto word = scancode_word(code & ~SDLK_SCANCODE_MASK);
        if (!word.empty())
            return word;
    } else if (printable(code)) {
        std::array<char, 8> utf8{};
        const char *end = SDL_UCS4ToUTF8(code, utf8.data());
        return {utf8.data(), static_cast<std::size_t>(end - utf8.data())};
    }
    return keycode_name(code);
}

// "keycode<n>": 1 to 10 digits, a non-zero u32.
std::optional<u32> keycode_from_word(std::string_view word) {
    constexpr std::string_view prefix = "keycode";
    if (!word.starts_with(prefix))
        return std::nullopt;
    const auto digits = word.substr(prefix.size());
    if (digits.empty() || digits.size() > 10)
        return std::nullopt;
    u32 code = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), code);
    if (error != std::errc{} || end != digits.data() + digits.size() || code == 0)
        return std::nullopt;
    return code;
}

// The word as one character, ASCII letters already lowercase as their SDLK_ values are.
std::optional<u32> code_point_from_word(const std::string &word) {
    const char *next = word.c_str();
    std::size_t left = word.size();
    const u32 code_point = SDL_StepUTF8(&next, &left);
    if (left != 0 || code_point == 0)
        return std::nullopt;
    return code_point;
}

std::optional<u32> scancode_key_from_word(std::string_view word) {
    for (u32 scancode = 1; scancode < SDL_SCANCODE_COUNT; ++scancode) {
        const auto name = scancode_word(scancode);
        if (!name.empty() && folded(name) == word)
            return SDL_SCANCODE_TO_KEYCODE(scancode);
    }
    return std::nullopt;
}

std::optional<Control> keys_from_word(const std::string &word) {
    for (const auto &entry : own_key_names)
        if (folded(entry.name) == word)
            return Control{InputDevice::keyboard, entry.code};
    auto code = keycode_from_word(word);
    if (!code)
        code = code_point_from_word(word);
    if (!code)
        code = scancode_key_from_word(word);
    if (code)
        return Control{InputDevice::keyboard, *code};
    if (const auto *mouse = find_word(mouse_names, word))
        return Control{InputDevice::mouse, mouse->code};
    return std::nullopt;
}

std::string key_name(u32 code) {
    for (const auto &entry : own_key_names)
        if (entry.code == code)
            return std::string(entry.name);
    if (code >= SDLK_A && code <= SDLK_Z)
        return std::string(1, static_cast<char>('A' + (code - SDLK_A)));
    // A name an earlier key has (Return2 is "Return" to SDL, 'A' reads as 'a') is not this key's.
    auto name = key_spelling(code);
    if (keys_from_word(folded(name)) != Control{InputDevice::keyboard, code})
        return keycode_name(code);
    return name;
}

std::string key_label(u32 code) {
    // A stored letter is a US place; show what that place types on this keymap.
    SDL_Keycode key = code;
    if (code >= SDLK_A && code <= SDLK_Z) {
        const auto letter_place = static_cast<SDL_Scancode>(SDL_SCANCODE_A + (code - SDLK_A));
        key = SDL_GetKeyFromScancode(letter_place, SDL_KMOD_NONE, false);
    }
    const char *label = SDL_GetKeyName(key);
    return label && *label ? std::string(label) : key_name(code);
}
} // namespace

std::string control_name(Control control) {
    switch (control.device) {
    case InputDevice::keyboard:
        return key_name(control.code);
    case InputDevice::mouse:
        if (const auto *entry = find_code(mouse_names, control.code))
            return std::string(entry->name);
        return {};
    case InputDevice::gamepad:
        if (const auto *entry = find_code(pad_names, control.code))
            return std::string(entry->name);
        return {};
    }
    return {};
}

std::optional<Control> control_from_name(BindingGroup group, std::string_view name) {
    if (name.empty() || name.size() > name_bytes_max)
        return std::nullopt;
    const auto word = folded(name);
    if (word.empty())
        return std::nullopt;
    std::optional<Control> found;
    if (group == BindingGroup::keys) {
        found = keys_from_word(word);
    } else if (const auto *entry = find_word(pad_names, word)) {
        found = Control{InputDevice::gamepad, entry->code};
    }
    // A name that would be written differently is not this control's.
    if (!found || folded(control_name(*found)) != word)
        return std::nullopt;
    return found;
}

bool control_bindable(BindingGroup group, Control control) {
    if (binding_group(control) != group || control_name(control).empty())
        return false;
    for (const auto &binding : fixed_bindings())
        if (binding.control == control)
            return false;
    // The overlay's own keys.
    const bool overlay_key = control.device == InputDevice::keyboard
        && (control.code == SDLK_F1 || control.code == SDLK_F11);
    return !overlay_key;
}

std::string controls_text(std::span<const Control> controls) {
    if (controls.empty())
        return "none";
    std::string text;
    for (const auto control : controls) {
        if (!text.empty())
            text += ',';
        text += control_name(control);
    }
    return text;
}

std::optional<ActionControls> controls_from_text(BindingGroup group, std::string_view text) {
    if (text.empty() || text.size() > text_bytes_max)
        return std::nullopt;
    if (folded(text) == "none")
        return ActionControls{};
    ActionControls controls;
    for (std::size_t start = 0;;) {
        const auto comma = text.find(',', start);
        const auto last = comma == std::string_view::npos;
        const auto piece = text.substr(start, last ? std::string_view::npos : comma - start);
        const auto control = control_from_name(group, piece);
        if (!control || !control_bindable(group, *control))
            return std::nullopt;
        if (!controls.has(*control)) {
            if (controls.count == controls.list.size())
                return std::nullopt;
            controls.list[controls.count++] = *control;
        }
        if (last)
            return controls;
        start = comma + 1;
    }
}

std::string control_label(Control control) {
    switch (control.device) {
    case InputDevice::keyboard:
        return key_label(control.code);
    case InputDevice::mouse:
        if (const auto *entry = find_code(mouse_names, control.code))
            return std::string(entry->label);
        return control_name(control);
    case InputDevice::gamepad:
        if (const auto *entry = find_code(pad_names, control.code))
            return std::string(entry->label);
        return control_name(control);
    }
    return control_name(control);
}
} // namespace kf
