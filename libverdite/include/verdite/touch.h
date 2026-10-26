#ifndef VERDITE_TOUCH_H
#define VERDITE_TOUCH_H
// On-screen controls for a touch screen: which finger is on which control, and
// what each control says at a moment. Game-agnostic: the game lays out its
// controls as a table (what each one is for is its own id), hands in the
// fingers, and reads the controls back as it reads a gamepad. It draws nothing
// and knows no windowing library; the caller draws each control from its row
// and its state.
//
// Places are fractions of the safe area (the screen less notches and system
// bars): x across from the left, y down from the top. Sizes are fractions of
// the safe area's height, so a control keeps its shape on any screen.
#include <verdite/stick.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace verdite {

enum class TouchKind : std::uint8_t {
    Stick,    // floating: centred where the finger lands in its zone; reads as a stick
    Button,   // held while the finger that landed on it stays down, wherever it slides
    Dpad,     // held towards where the finger is from its centre, eight ways
    Drag,     // reads as how far its finger moved, as a mouse does (looking around)
    Tap,      // a press when a touch lifts soon, near where it landed
};

struct TouchArea {
    float x = 0, y = 0, width = 0, height = 0;   // fractions of the safe area
};

struct TouchControl {
    std::uint32_t id = 0;   // the game's: an action, a stick, a look area
    TouchKind kind = TouchKind::Button;
    TouchArea zone;         // a finger landing here takes the control; the first row that holds it wins
    float radius = 0;       // Stick: full throw; Button and Dpad: as drawn (fractions of the height)
};

// What a control says now.
struct TouchState {
    bool held = false;            // a finger is on it
    StickAxes stick;              // Stick: by throw, -1..1; Dpad: -1, 0 or 1 an axis
    float drag_x_px = 0;          // Drag: moved since the last take_frame
    float drag_y_px = 0;
    bool tapped = false;          // Tap: since the last take_frame
    float centre_x = 0;           // Stick: where it was centred, for drawing (fractions)
    float centre_y = 0;
};

class TouchControls {
public:
    // The safe area in window pixels, where the fractions are measured.
    void set_safe_area(float x_px, float y_px, float width_px, float height_px);
    // Replaces the controls (another screen: walking, a menu). Fingers already
    // down keep nothing; they take a control by landing again.
    void set_layout(std::span<const TouchControl> controls);

    // SDL's finger events, in window pixels. A finger landing on no control is
    // ignored until it lifts.
    void finger_down(std::uint64_t finger, float x_px, float y_px, std::uint64_t at_ms);
    void finger_moved(std::uint64_t finger, float x_px, float y_px);
    void finger_up(std::uint64_t finger, std::uint64_t at_ms);
    // Every finger lifted at once: focus lost, the app sent to the background.
    void release_all();

    // Each control's state, in layout order. Drags and taps are reset by the
    // call, so each counts once; held, sticks and D-pads stay until fingers move.
    std::span<const TouchState> take_frame();
    // Whether any finger is on a control: the game shows the controls again
    // after a gamepad hid them.
    bool any_held() const;

private:
    struct Finger {
        std::uint64_t id;
        std::size_t control;          // index into controls_
        float down_x_px, down_y_px;   // where it landed
        float last_x_px, last_y_px;
        std::uint64_t down_ms;
    };
    std::vector<TouchControl> controls_;
    std::vector<TouchState> states_;
    std::vector<Finger> fingers_;
    float area_x_px_ = 0, area_y_px_ = 0, area_width_px_ = 1, area_height_px_ = 1;
};

} // namespace verdite

#endif // VERDITE_TOUCH_H
