#ifndef KF_EXT_SETUP_OVERLAY_H
#define KF_EXT_SETUP_OVERLAY_H
// The first-run setup as a Dear ImGui window: what a desktop draws for verdite::SetupScreen.
#include <verdite/setup.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

namespace kf::ext {

// A control the player used. Dear ImGui draws every frame, so the screen keeps the last view and a
// control waits until the frame is drawn, since running it replaces the view being drawn.
using SetupControl = std::function<void(verdite::Setup &)>;

class SetupOverlayScreen final : public verdite::SetupScreen {
public:
    void show(const verdite::SetupPageView &view) override;
    void close() override { view_.reset(); }

    // Inside the ImGui frame: draws the window and adds what the player did to controls.
    void draw(std::vector<SetupControl> &controls);

private:
    std::optional<verdite::SetupPageView> view_;
    bool focus_first_ = false;
    bool pad_in_use_ = false;   // the device used last, for the foot's hints
    bool list_was_open_ = false;   // a list or a held slider had the input last frame
};

} // namespace kf::ext

#endif // KF_EXT_SETUP_OVERLAY_H
