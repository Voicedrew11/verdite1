// The touch controls drawn over the game, translucent, from what
// kf/platform/touch.h says is on screen, on the interface's front canvas.
#include "internal.hpp"
#include <kf/platform/touch.h>
#include <verdite/ui_canvas.h>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace kf::ext {
namespace {

using verdite::ui::Canvas;
using verdite::ui::Colour;
using verdite::ui::Fill;
using verdite::ui::Point;

// The controls' colours at an opacity, 0..1, as they fade in and out.
struct Palette {
    Colour fill, fill_held, edge, text;
};

Colour faded(int r, int g, int b, int alpha, float opacity) {
    return {static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f,
            static_cast<float>(alpha) / 255.0f * opacity};
}

Palette palette(float opacity) {
    return {faded(20, 20, 24, 90, opacity), faded(230, 210, 160, 120, opacity), faded(235, 225, 200, 150, opacity),
            faded(245, 238, 220, 210, opacity)};
}

void draw_button(Canvas &canvas, const Palette &colors, const TouchShown &shown) {
    const Point at{shown.x_px, shown.y_px};
    const float radius = shown.radius_px;
    canvas.circle(at, radius, shown.held ? colors.fill_held : colors.fill, Fill::Solid);
    canvas.circle(at, radius, colors.edge, Fill::Outline, std::max(1.5f, radius * 0.05f));
    if (!shown.label)
        return;
    const float size = std::max(12.0f, radius * 0.42f);
    const Point extent = canvas.text_size(shown.label, size);
    canvas.text({at.x - extent.x * 0.5f, at.y - extent.y * 0.5f}, shown.label, colors.text, size);
}

void draw_gear(Canvas &canvas, const Palette &colors, const TouchShown &shown) {
    draw_button(canvas, colors, shown);
    const Point at{shown.x_px, shown.y_px};
    const float ring = shown.radius_px * 0.38f;
    const float thickness = std::max(2.0f, shown.radius_px * 0.12f);
    canvas.circle(at, ring, colors.text, Fill::Outline, thickness);
    constexpr int teeth = 8;
    for (int i = 0; i < teeth; ++i) {
        const float angle = static_cast<float>(i) * 2 * std::numbers::pi_v<float> / teeth;
        const Point way{std::cos(angle), std::sin(angle)};
        canvas.line({at.x + way.x * ring, at.y + way.y * ring},
                    {at.x + way.x * ring * 1.6f, at.y + way.y * ring * 1.6f}, colors.text, thickness);
    }
}

void draw_stick(Canvas &canvas, const Palette &colors, const TouchShown &shown) {
    const Point base{shown.x_px, shown.y_px};
    const float radius = shown.radius_px;
    canvas.circle(base, radius, colors.fill, Fill::Solid);
    canvas.circle(base, radius, colors.edge, Fill::Outline, std::max(1.5f, radius * 0.04f));
    canvas.circle({shown.knob_x_px, shown.knob_y_px}, radius * 0.45f, shown.held ? colors.fill_held : colors.edge,
                  Fill::Solid);
}

} // namespace

// Touch positions are the window's pixels, as the canvas's are.
void touch_controls_draw(Canvas &canvas) {
    const Palette colors = palette(host_touch_opacity());
    for (const TouchShown &shown : host_touch_shown()) {
        switch (shown.shape) {
        case TouchShape::Stick: draw_stick(canvas, colors, shown); break;
        case TouchShape::Button: draw_button(canvas, colors, shown); break;
        case TouchShape::Gear: draw_gear(canvas, colors, shown); break;
        }
    }
}

void touch_commands_poll() {
    switch (host_touch_take_command()) {
    case TouchCommand::Settings:
        if (!settings_panel_show())
            overlay_set_open(true);
        break;
    case TouchCommand::None: break;
    }
}

} // namespace kf::ext
