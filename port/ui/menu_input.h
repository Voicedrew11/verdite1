#ifndef KF_UI_MENU_INPUT_H
#define KF_UI_MENU_INPUT_H
// Keys, pad buttons and the left stick as a menu's inputs (verdite/menu_model.h),
// with a direction held on the pad or the stick repeated. Confirm and back sit
// where the player's pad layout has them. Game thread.
#include <SDL3/SDL.h>
#include <verdite/menu_model.h>

#include <optional>

namespace kf::ext {

class MenuInputs {
public:
    // The input the event makes, if any. A key repeats by its own repeat.
    [[nodiscard]] std::optional<verdite::MenuInput> take(const SDL_Event &event);
    // Each frame: a held direction again, once it is due.
    [[nodiscard]] std::optional<verdite::MenuInput> repeat();
    // Nothing held (the menu opened or closed).
    void reset();

private:
    std::optional<verdite::MenuInput> hold(verdite::MenuInput input);
    void let_go(verdite::MenuInput input);
    std::optional<verdite::MenuInput> stick(const SDL_GamepadAxisEvent &axis);

    std::optional<verdite::MenuInput> held_;
    Uint64 held_next_ms_ = 0;
    std::optional<verdite::MenuInput> stick_x_, stick_y_;   // the stick's direction on each axis
};

} // namespace kf::ext

#endif // KF_UI_MENU_INPUT_H
