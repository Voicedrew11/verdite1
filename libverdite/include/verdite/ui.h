#ifndef VERDITE_UI_H
#define VERDITE_UI_H
// The interface over the game: RmlUi documents (RML laid out by RCSS style
// sheets) and two canvases of free shapes, recorded each frame as triangles
// (verdite/ui_frame.h) for the renderer backend to draw. From holyland's player
// menu and launcher (port/ui/, launcher/window.cpp). RmlUi types stay inside
// libverdite's ui sources.
//
// The game hands in its files (documents, style sheets, fonts) carried in the
// binary (cmake/ui_assets.cmake), folders on disk for its mods, the fonts to
// load, its images, and a host for the clock, the pointer's shape, the
// clipboard and the log. It feeds the platform's input in, draws a frame for
// each present, and reads back what the player pressed as Actions.
//
// Elements say what pressing them does with attributes, read on the element or
// its nearest parent that has one:
//   data-action="name" data-value="x"   a press (mouse button down, or a tap
//                                        that did not scroll) gives Action
//                                        {Press, name, value}
//   data-hover="name" data-value="x"    the pointer coming over it gives
//                                        {Hover, name, value}
//   data-enter="name"                    Enter in a text field gives {Enter}
// A press acts on the button going down, not on release, because a menu
// rewrites its RML between the two.
//
// One Screen at a time (RmlUi is one library-wide state); game thread only.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <verdite/ui_canvas.h>
#include <verdite/ui_frame.h>

namespace verdite::ui {

// A file carried in the binary, named by its path as documents name it
// ("menu.rml", "fonts/menu.ttf").
struct Asset {
    std::string_view path;
    std::span<const std::byte> bytes;
};

struct FontFace {
    std::string_view asset_path;
    std::string_view family;   // as the style sheets name it
    bool bold = false;
};

// Images the documents name (<img src>, decorator images) that are not files:
// keycaps, the game's own art. Nothing when the source is not one of its own.
// A source it has nothing for is then looked for among the carried alpha masks.
class ImageSource {
public:
    virtual ~ImageSource() = default;
    [[nodiscard]] virtual std::optional<TextureUpload> load(std::string_view source) const = 0;
};

// A carried alpha mask (holyland's tools/ui_masks.py makes them: logos,
// ornaments): "KF4A", the width and the height as little-endian 32-bit
// numbers, then a byte of alpha for each pixel, rows top first. As a white
// texture, premultiplied, for the style sheet to colour (image-color). Nothing
// when the bytes are not one whole mask. Documents name a mask by its asset
// path ("logo.alpha").
[[nodiscard]] std::optional<TextureUpload> alpha_mask(std::span<const std::byte> file);

enum class Cursor : unsigned char { Arrow, Hand, Text };

// What the platform does for RmlUi.
class ScreenHost {
public:
    virtual ~ScreenHost() = default;
    [[nodiscard]] virtual double seconds() const = 0;   // a steady clock
    virtual void set_cursor(Cursor cursor) = 0;
    virtual void set_clipboard(std::string_view utf8) = 0;
    [[nodiscard]] virtual std::string clipboard() const = 0;
    virtual void log(std::string_view line) = 0;   // RmlUi's warnings and errors
};

struct ScreenSetup {
    ScreenHost *host = nullptr;                 // outlives the Screen
    std::span<const Asset> assets;              // outlive the Screen
    std::span<const FontFace> fonts;            // the first is the default face
    const ImageSource *images = nullptr;        // may be null; outlives the Screen
};

enum class ActionKind : unsigned char { Press, Hover, Enter };

struct Action {
    ActionKind kind = ActionKind::Press;
    std::string document;   // the asset path it was loaded from
    std::string name;
    std::string value;
};

// The keys text fields and their selection use; the platform maps its own.
enum class TextKey : unsigned char {
    Backspace, Delete, Left, Right, Up, Down, Home, End, Enter, Tab, Escape,
    SelectAll, Copy, Cut, Paste,
};
enum class KeyMods : unsigned char { None = 0, Shift = 1 };

class Document {
public:
    // A part by its element id: its inner RML, its text (escaped) or its
    // classes. Each reaches RmlUi only when it changed; an id the document
    // does not have is left alone.
    void set_rml(std::string_view id, std::string_view rml);
    void set_text(std::string_view id, std::string_view text);
    void set_classes(std::string_view id, std::string_view classes);
    // The body's classes (a document-wide state: "touch", "narrow").
    void set_body_classes(std::string_view classes);
    // A text field's value (an <input> by its id), and setting it.
    [[nodiscard]] std::string field(std::string_view id) const;
    void set_field(std::string_view id, std::string_view value);
    // Keyboard focus into a text field, or away from every field.
    void focus_field(std::string_view id);
    void blur();
    void scroll_to_end(std::string_view id);
    void scroll_into_view(std::string_view id);

    void show();
    void hide();
    [[nodiscard]] bool shown() const;
    // Over the other documents, or under them (the canvases stay outermost).
    void raise();
    void lower();

    struct State;   // ui.cpp
    explicit Document(std::unique_ptr<State> state);
    ~Document();
    Document(const Document &) = delete;
    Document &operator=(const Document &) = delete;

private:
    std::unique_ptr<State> state_;
};

class Screen {
public:
    // Starts RmlUi with the setup's files and fonts; null, with the reason
    // logged, if RmlUi, its context or a font fails.
    [[nodiscard]] static std::unique_ptr<Screen> open(const ScreenSetup &setup);
    ~Screen();
    Screen(const Screen &) = delete;
    Screen &operator=(const Screen &) = delete;

    // Paths starting "<prefix>/" come from that folder on disk, and never from
    // outside it (no "..", no link out): a mod's documents. Replaces an earlier
    // folder of the same prefix.
    void add_folder(std::string_view prefix, const std::filesystem::path &root);
    void remove_folder(std::string_view prefix);

    // A document by path (an asset, or under a folder), loaded hidden the
    // first time and kept; null, logged, when it does not load. Borrowed.
    Document *load(std::string_view path);
    // Lets a document go (a mod reloaded): its pointer is no longer valid.
    void unload(std::string_view path);

    // The frame's size and how many pixels a dp is (the display's scale times
    // the player's interface scale): RmlUi's dp unit and the default font size
    // follow it.
    void resize(int width_px, int height_px, float px_per_dp);
    [[nodiscard]] float px_per_dp() const;

    void pointer_move(float x_px, float y_px);
    void pointer_down(float x_px, float y_px);
    void pointer_up(float x_px, float y_px);
    void pointer_leave();
    void wheel(float steps_down);
    // A finger: a tap presses, a drag scrolls what is under it. One finger at a
    // time; others are ignored until it lifts.
    void touch_down(std::int64_t finger, float x_px, float y_px);
    void touch_move(std::int64_t finger, float x_px, float y_px);
    void touch_up(std::int64_t finger, float x_px, float y_px);
    void key(TextKey key, KeyMods mods);
    void text_input(std::string_view utf8);
    // Whether a text field has the keyboard focus (start the platform's text
    // input, show a phone's keyboard).
    [[nodiscard]] bool wants_text() const;
    // Whether the pointer is over an element of a shown document (other than
    // its body): the game should not take that click.
    [[nodiscard]] bool pointer_over_document() const;

    // The next thing the player pressed since the last draw, oldest first.
    [[nodiscard]] std::optional<Action> take_action();

    [[nodiscard]] Canvas &canvas(Layer layer);

    // Lays out and records the frame: the canvases' shapes since the last
    // draw, then every shown document. Valid until the next draw.
    const Frame &draw();

    struct State;   // ui.cpp

private:
    explicit Screen(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

} // namespace verdite::ui

#endif // VERDITE_UI_H
