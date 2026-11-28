#ifndef VERDITE_UI_CANVAS_COMMANDS_H
#define VERDITE_UI_CANVAS_COMMANDS_H
// What a Canvas recorded (verdite/ui_canvas.h), for the Screen's canvas
// element (ui.cpp): shapes already as triangles, text still as text.
#include <cstdint>
#include <string>
#include <vector>

#include <verdite/ui_canvas.h>
#include <verdite/ui_frame.h>

namespace verdite::ui {

enum class PieceKind : unsigned char { Triangles, Text, PushClip, PopClip };

struct CanvasPiece {
    PieceKind kind = PieceKind::Triangles;
    std::vector<Vertex> vertices;        // Triangles: premultiplied, untextured
    std::vector<std::uint32_t> indices;
    std::string text;                    // Text
    std::string family;
    Point at;
    Colour colour;
    float size_px = 0;
    Area clip;                           // PushClip
};

struct Canvas::Commands {
    std::vector<CanvasPiece> pieces;
};

} // namespace verdite::ui

#endif // VERDITE_UI_CANVAS_COMMANDS_H
