// The touch controls drawn over the game, translucent, from what
// kf/platform/touch.h says is on screen.
#include "internal.hpp"
#include <imgui.h>
#include <kf/platform/touch.h>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace kf::ext {
namespace {

// The controls' colours at an opacity, 0..1, as they fade in and out.
struct Palette {
    ImU32 fill, fill_held, edge, text;
};

ImU32 faded(int r, int g, int b, int alpha, float opacity) {
    return IM_COL32(r, g, b, static_cast<int>(std::lround(static_cast<float>(alpha) * opacity)));
}

Palette palette(float opacity) {
    return {faded(20, 20, 24, 90, opacity), faded(230, 210, 160, 120, opacity), faded(235, 225, 200, 150, opacity),
            faded(245, 238, 220, 210, opacity)};
}

void draw_button(ImDrawList *draw, const Palette &colors, const TouchShown &shown) {
    const ImVec2 at(shown.x_px, shown.y_px);
    draw->AddCircleFilled(at, shown.radius_px, shown.held ? colors.fill_held : colors.fill);
    draw->AddCircle(at, shown.radius_px, colors.edge, 0, std::max(1.5f, shown.radius_px * 0.05f));
    if (!shown.label)
        return;
    ImFont *font = ImGui::GetFont();
    const float size = std::max(12.0f, shown.radius_px * 0.42f);
    const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, shown.label);
    draw->AddText(font, size, ImVec2(at.x - extent.x * 0.5f, at.y - extent.y * 0.5f), colors.text, shown.label);
}

void draw_gear(ImDrawList *draw, const Palette &colors, const TouchShown &shown) {
    draw_button(draw, colors, shown);
    const ImVec2 at(shown.x_px, shown.y_px);
    const float ring = shown.radius_px * 0.38f;
    const float thickness = std::max(2.0f, shown.radius_px * 0.12f);
    draw->AddCircle(at, ring, colors.text, 0, thickness);
    constexpr int teeth = 8;
    for (int i = 0; i < teeth; ++i) {
        const float angle = static_cast<float>(i) * 2 * std::numbers::pi_v<float> / teeth;
        const ImVec2 way(std::cos(angle), std::sin(angle));
        draw->AddLine(ImVec2(at.x + way.x * ring, at.y + way.y * ring),
                      ImVec2(at.x + way.x * ring * 1.6f, at.y + way.y * ring * 1.6f), colors.text, thickness);
    }
}

void draw_stick(ImDrawList *draw, const Palette &colors, const TouchShown &shown) {
    const ImVec2 base(shown.x_px, shown.y_px);
    draw->AddCircleFilled(base, shown.radius_px, colors.fill);
    draw->AddCircle(base, shown.radius_px, colors.edge, 0, std::max(1.5f, shown.radius_px * 0.04f));
    draw->AddCircleFilled(ImVec2(shown.knob_x_px, shown.knob_y_px), shown.radius_px * 0.45f,
                          shown.held ? colors.fill_held : colors.edge);
}

} // namespace

void touch_controls_draw() {
    ImDrawList *draw = ImGui::GetForegroundDrawList();
    const Palette colors = palette(host_touch_opacity());
    for (const TouchShown &shown : host_touch_shown()) {
        switch (shown.shape) {
        case TouchShape::Stick: draw_stick(draw, colors, shown); break;
        case TouchShape::Button: draw_button(draw, colors, shown); break;
        case TouchShape::Gear: draw_gear(draw, colors, shown); break;
        }
    }
}

void touch_commands_poll() {
    switch (host_touch_take_command()) {
    case TouchCommand::Settings:
        if (!settings_panel_show())
            overlay_set_open(true);
        break;
    case TouchCommand::FullMap: automap_toggle_full(); break;
    case TouchCommand::None: break;
    }
}

} // namespace kf::ext
