#include <kf/platform/menu_pointer.h>

#include <kf/platform/input.h>
#include <kf/renderer/constants.h>
#include <kf/renderer/frontend.h>
#include <verdite/menu_pointer.h>

#include <SDL3/SDL.h>

#include <array>

namespace kf {
namespace {

verdite::MenuPointer pointer;

// A window position (SDL's, in points) in the original 320 x 240 pixels.
std::array<float, 2> game_position(SDL_Window *window, float x, float y) {
    int width_px = 0, height_px = 0;
    if (!SDL_GetWindowSizeInPixels(window, &width_px, &height_px) || width_px <= 0 || height_px <= 0)
        return {-1, -1};
    const float density = SDL_GetWindowPixelDensity(window);
    const auto picture = renderer_present_rect(width_px, height_px);
    if (picture.width <= 0 || picture.height <= 0 || density <= 0)
        return {-1, -1};
    const int extra = renderer_extra_columns();
    const float column_px = static_cast<float>(picture.width) / static_cast<float>(render_width + 2 * extra);
    const float row_px = static_cast<float>(picture.height) / static_cast<float>(render_height);
    return {(x * density - static_cast<float>(picture.x)) / column_px - static_cast<float>(extra),
            (y * density - static_cast<float>(picture.y)) / row_px};
}

void mouse_button(SDL_Window *window, const SDL_MouseButtonEvent &button) {
    const auto [x, y] = game_position(window, button.x, button.y);
    if (button.button == SDL_BUTTON_LEFT) {
        if (button.down)
            pointer.pressed(x, y);
        else
            pointer.released(x, y);
    } else if (button.button == SDL_BUTTON_RIGHT && button.down) {
        pointer.back();
    }
}

} // namespace

void host_menu_widget(MenuWidget widget, s32 cursor_row) {
    pointer.begin_widget(static_cast<std::uint32_t>(widget), cursor_row);
}

bool host_menu_row(s32 row, s32 x, s32 y, s32 width, s32 height) {
    return pointer.add_row(row, {static_cast<float>(x), static_cast<float>(y), static_cast<float>(width),
                                 static_cast<float>(height)});
}

std::optional<s32> host_menu_pointed_row(MenuWidget widget) {
    return pointer.take_row(static_cast<std::uint32_t>(widget));
}

void host_menu_pointer_event(SDL_Window *window, const SDL_Event &event) {
    if (!menu_pointer_enabled || !window || SDL_GetWindowRelativeMouseMode(window))
        return;
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
        if (event.motion.which != SDL_TOUCH_MOUSEID) {
            const auto [x, y] = game_position(window, event.motion.x, event.motion.y);
            pointer.moved(x, y);
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event.button.which != SDL_TOUCH_MOUSEID)
            mouse_button(window, event.button);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        // Positive integer_y is away from the user: up the list.
        if (event.wheel.which != SDL_TOUCH_MOUSEID && event.wheel.integer_y != 0)
            pointer.scroll(-event.wheel.integer_y);
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        pointer.gone();
        break;
    default:
        break;
    }
}

void host_menu_pointer_gone() { pointer = {}; }

verdite::MenuPointer &host_menu_pointer() { return pointer; }

void host_menu_frame_begin() { pointer.begin_frame(); }

void host_menu_frame_shown() { pointer.frame_shown(); }

u32 host_menu_pointer_buttons() {
    if (!menu_pointer_enabled)
        return 0;
    switch (pointer.take_press()) {
    case verdite::MenuPress::None: return 0;
    case verdite::MenuPress::Confirm: return button_mask(Button::Confirm);
    case verdite::MenuPress::Back: return button_mask(Button::Back);
    case verdite::MenuPress::Up: return button_mask(Button::Up);
    case verdite::MenuPress::Down: return button_mask(Button::Down);
    }
    return 0;
}
} // namespace kf
