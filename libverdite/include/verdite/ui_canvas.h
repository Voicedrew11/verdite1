#ifndef VERDITE_UI_CANVAS_H
#define VERDITE_UI_CANVAS_H
// Shapes and text drawn freely over the game, a frame at a time: the parts of
// an interface that are not documents (a toast, a frame counter, on-screen
// touch controls, a mod's drawing). A Screen (verdite/ui.h) has two, one under
// its documents and one over them; what is drawn on one shows in the next
// frame the Screen draws, then it is cleared. Positions and sizes are in the
// frame's pixels; text is in the Screen's fonts.
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace verdite::ui {

struct Point {
    float x = 0, y = 0;
};

struct Area {
    float x = 0, y = 0, width = 0, height = 0;
};

// Straight (not premultiplied) colour, each 0 to 1.
struct Colour {
    float r = 1, g = 1, b = 1, a = 1;
};

enum class Layer : unsigned char {
    Back,    // under every document
    Front,   // over every document
};

enum class Fill : unsigned char { Solid, Outline };

// A drop shadow is a copy one pixel down and right, black at the colour's alpha.
enum class Shadow : unsigned char { None, Drop };

// How wide and tall a line of text is in a face and size (the Screen's fonts).
class TextMeasure {
public:
    virtual ~TextMeasure() = default;
    [[nodiscard]] virtual Point text_size(std::string_view utf8, float size_px,
                                          std::string_view family) const = 0;
};

class Canvas {
public:
    explicit Canvas(const TextMeasure &measure);
    ~Canvas();
    Canvas(const Canvas &) = delete;
    Canvas &operator=(const Canvas &) = delete;

    // An outline is thickness_px wide, inside the shape's edge for a rect.
    void rect(Area area, Colour colour, Fill fill, float thickness_px = 1);
    void rounded_rect(Area area, float radius_px, Colour colour);
    void line(Point from, Point to, Colour colour, float thickness_px = 1);
    void circle(Point centre, float radius_px, Colour colour, Fill fill, float thickness_px = 1);
    // A convex polygon, or a closed outline; fewer than 3 points draw nothing.
    void polygon(std::span<const Point> points, Colour colour, Fill fill, float thickness_px = 1);
    // One line of UTF-8 text with its top left at `at`, in a family the Screen
    // loaded (empty: its first) and size.
    void text(Point at, std::string_view utf8, Colour colour, float size_px,
              Shadow shadow = Shadow::None, std::string_view family = {});
    // What text would cover: its width and its line height.
    [[nodiscard]] Point text_size(std::string_view utf8, float size_px,
                                  std::string_view family = {}) const;
    // Text broken into lines no wider than width_px, at spaces and line
    // breaks (a word wider than the width has a line of its own).
    [[nodiscard]] std::vector<std::string> wrap(std::string_view utf8, float width_px, float size_px,
                                                std::string_view family = {}) const;
    // Later shapes only inside the area (and the clips under it); pops past the
    // first are ignored.
    void push_clip(Area area);
    void pop_clip();
    // True when nothing was drawn since the Screen last took the commands.
    [[nodiscard]] bool empty() const;

    struct Commands;   // ui_canvas.cpp; read by the Screen's canvas element
    // The commands since the last take; the canvas starts empty again.
    [[nodiscard]] std::unique_ptr<Commands> take();

private:
    const TextMeasure &measure_;
    std::unique_ptr<Commands> commands_;
    int clip_depth_ = 0;
};

} // namespace verdite::ui

#endif // VERDITE_UI_CANVAS_H
