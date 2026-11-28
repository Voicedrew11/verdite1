#ifndef KF_UI_SETUP_SCREEN_H
#define KF_UI_SETUP_SCREEN_H
// The first-run setup drawn in RmlUi (port/ui/res/setup.rml): what every
// platform with the interface shows for verdite::SetupScreen. The welcome's
// presets, a page's groups and options and the summary's lines are rows of
// one list, moved about as the F1 menu's are (verdite/menu_model.h), with Back
// and Next (Finish, Skip) as its last rows. Game thread.
#include <SDL3/SDL.h>
#include <verdite/setup.h>

#include <memory>

namespace verdite::ui {
struct Action;
}

namespace kf::ext {

class SetupMenuScreen final : public verdite::SetupScreen {
public:
    SetupMenuScreen();
    ~SetupMenuScreen() override;
    SetupMenuScreen(const SetupMenuScreen &) = delete;
    SetupMenuScreen &operator=(const SetupMenuScreen &) = delete;

    void show(const verdite::SetupPageView &view) override;
    void close() override;

    // The Setup the controls go to while it runs; null when it ends.
    void attach(verdite::Setup *setup);
    // Keys and the pad while it is open: true when it took them.
    bool event(const SDL_Event &event);
    // A press or hover on its document.
    void action(const verdite::ui::Action &action);
    // Each frame before the interface is drawn.
    void frame();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace kf::ext

#endif // KF_UI_SETUP_SCREEN_H
