// Shapes and text drawn freely (verdite/ui_canvas.h): each shape made into
// triangles here, text left for the Screen's fonts.
#include <verdite/ui_canvas.h>

#include <algorithm>
#include <cmath>
#include <numbers>

#include "ui_canvas_commands.h"

namespace verdite::ui {

namespace {

constexpr int circle_segments_min = 12;
constexpr int circle_segments_max = 64;
constexpr float pixels_per_segment = 4;
constexpr float rounded_corner_segments = 6;

std::uint8_t channel(float value) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

Vertex vertex(Point at, Colour colour) {
    float a = std::clamp(colour.a, 0.0f, 1.0f);
    return {at.x, at.y, channel(colour.r * a), channel(colour.g * a), channel(colour.b * a),
            channel(a), 0, 0};
}

// A piece's triangles fanned from its first vertex over the rest.
void fan(CanvasPiece &piece, std::span<const Point> points, Colour colour) {
    auto base = static_cast<std::uint32_t>(piece.vertices.size());
    for (Point point : points) {
        piece.vertices.push_back(vertex(point, colour));
    }
    for (std::uint32_t i = 1; i + 1 < points.size(); i++) {
        piece.indices.insert(piece.indices.end(), {base, base + i, base + i + 1});
    }
}

void quad(CanvasPiece &piece, Point a, Point b, Point c, Point d, Colour colour) {
    const Point corners[] = {a, b, c, d};
    fan(piece, corners, colour);
}

// A band thickness wide along from-to.
void band(CanvasPiece &piece, Point from, Point to, Colour colour, float thickness) {
    float dx = to.x - from.x;
    float dy = to.y - from.y;
    float length = std::hypot(dx, dy);
    if (length <= 0) {
        return;
    }
    float nx = -dy / length * thickness / 2;
    float ny = dx / length * thickness / 2;
    quad(piece, {from.x + nx, from.y + ny}, {to.x + nx, to.y + ny}, {to.x - nx, to.y - ny},
         {from.x - nx, from.y - ny}, colour);
}

void outline(CanvasPiece &piece, std::span<const Point> points, Colour colour, float thickness) {
    for (size_t i = 0; i < points.size(); i++) {
        band(piece, points[i], points[(i + 1) % points.size()], colour, thickness);
    }
}

int circle_segments(float radius) {
    int wanted = static_cast<int>(2 * std::numbers::pi_v<float> * radius / pixels_per_segment);
    return std::clamp(wanted, circle_segments_min, circle_segments_max);
}

// Points around an arc from start_angle over sweep, radius from centre.
void arc(std::vector<Point> &points, Point centre, float radius, float start_angle, float sweep,
         int segments) {
    for (int i = 0; i <= segments; i++) {
        float angle = start_angle + sweep * static_cast<float>(i) / static_cast<float>(segments);
        points.push_back({centre.x + std::cos(angle) * radius, centre.y + std::sin(angle) * radius});
    }
}

} // namespace

Canvas::Canvas(const TextMeasure &measure)
    : measure_(measure), commands_(std::make_unique<Commands>()) {}

Canvas::~Canvas() = default;

void Canvas::rect(Area area, Colour colour, Fill fill, float thickness_px) {
    if (area.width <= 0 || area.height <= 0) {
        return;
    }
    CanvasPiece &piece = commands_->pieces.emplace_back();
    float left = area.x;
    float top = area.y;
    float right = area.x + area.width;
    float bottom = area.y + area.height;
    if (fill == Fill::Solid) {
        quad(piece, {left, top}, {right, top}, {right, bottom}, {left, bottom}, colour);
        return;
    }
    float t = std::min({thickness_px, area.width / 2, area.height / 2});
    quad(piece, {left, top}, {right, top}, {right, top + t}, {left, top + t}, colour);
    quad(piece, {left, bottom - t}, {right, bottom - t}, {right, bottom}, {left, bottom}, colour);
    quad(piece, {left, top + t}, {left + t, top + t}, {left + t, bottom - t}, {left, bottom - t},
         colour);
    quad(piece, {right - t, top + t}, {right, top + t}, {right, bottom - t},
         {right - t, bottom - t}, colour);
}

void Canvas::rounded_rect(Area area, float radius_px, Colour colour) {
    float radius = std::clamp(radius_px, 0.0f, std::min(area.width, area.height) / 2);
    if (radius <= 0) {
        rect(area, colour, Fill::Solid);
        return;
    }
    constexpr float quarter = std::numbers::pi_v<float> / 2;
    auto segments = static_cast<int>(rounded_corner_segments);
    std::vector<Point> points;
    float left = area.x + radius;
    float right = area.x + area.width - radius;
    float top = area.y + radius;
    float bottom = area.y + area.height - radius;
    arc(points, {right, top}, radius, -quarter, quarter, segments);
    arc(points, {right, bottom}, radius, 0, quarter, segments);
    arc(points, {left, bottom}, radius, quarter, quarter, segments);
    arc(points, {left, top}, radius, 2 * quarter, quarter, segments);
    fan(commands_->pieces.emplace_back(), points, colour);
}

void Canvas::line(Point from, Point to, Colour colour, float thickness_px) {
    band(commands_->pieces.emplace_back(), from, to, colour, std::max(thickness_px, 0.0f));
}

void Canvas::circle(Point centre, float radius_px, Colour colour, Fill fill, float thickness_px) {
    if (radius_px <= 0) {
        return;
    }
    int segments = circle_segments(radius_px);
    std::vector<Point> points;
    arc(points, centre, radius_px, 0, 2 * std::numbers::pi_v<float>, segments);
    points.pop_back();   // (the last is the first again)
    polygon(points, colour, fill, thickness_px);
}

void Canvas::polygon(std::span<const Point> points, Colour colour, Fill fill, float thickness_px) {
    if (points.size() < 3) {
        return;
    }
    CanvasPiece &piece = commands_->pieces.emplace_back();
    if (fill == Fill::Solid) {
        fan(piece, points, colour);
    } else {
        outline(piece, points, colour, std::max(thickness_px, 0.0f));
    }
}

void Canvas::text(Point at, std::string_view utf8, Colour colour, float size_px, Shadow shadow,
                  std::string_view family) {
    if (utf8.empty() || size_px <= 0) {
        return;
    }
    if (shadow == Shadow::Drop) {
        text({at.x + 1, at.y + 1}, utf8, {0, 0, 0, colour.a}, size_px, Shadow::None, family);
    }
    CanvasPiece &piece = commands_->pieces.emplace_back();
    piece.kind = PieceKind::Text;
    piece.text = utf8;
    piece.family = family;
    piece.at = at;
    piece.colour = colour;
    piece.size_px = size_px;
}

Point Canvas::text_size(std::string_view utf8, float size_px, std::string_view family) const {
    return measure_.text_size(utf8, size_px, family);
}

void Canvas::push_clip(Area area) {
    CanvasPiece &piece = commands_->pieces.emplace_back();
    piece.kind = PieceKind::PushClip;
    piece.clip = area;
    clip_depth_++;
}

void Canvas::pop_clip() {
    if (clip_depth_ == 0) {
        return;
    }
    commands_->pieces.emplace_back().kind = PieceKind::PopClip;
    clip_depth_--;
}

bool Canvas::empty() const {
    return commands_->pieces.empty();
}

std::unique_ptr<Canvas::Commands> Canvas::take() {
    for (; clip_depth_ > 0; clip_depth_--) {
        commands_->pieces.emplace_back().kind = PieceKind::PopClip;   // clips left open end here
    }
    auto taken = std::move(commands_);
    commands_ = std::make_unique<Commands>();
    return taken;
}

} // namespace verdite::ui
