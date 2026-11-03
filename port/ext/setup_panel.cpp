// Android's setup screen: native Material views over the game, drawn by SetupPanel.java from the
// setup's JSON. The panel lives on Android's UI thread; its taps cross to the game thread as
// Setup::command lines and the view comes back as a string, both under one mutex. Elsewhere there
// is no panel, and setup_overlay.cpp draws the setup.
#include "internal.hpp"

#include <string>
#include <utility>
#include <vector>

#ifdef __ANDROID__
#include <jni.h>

#include <mutex>

namespace kf::ext {
namespace {
// Shared with the UI thread.
struct Shared {
    std::mutex lock;
    std::vector<std::string> controls;
    std::string view_json;
};
Shared shared;

// Called on the game thread.
class PanelScreen final : public verdite::SetupScreen {
public:
    void show(const verdite::SetupPageView &view) override {
        {
            std::string json = verdite::setup_json(view);
            std::lock_guard guard(shared.lock);
            shared.view_json = std::move(json);
        }
        android_call_activity("showSetupPanel");
    }
    void close() override { android_call_activity("closeSetupPanel"); }
};
PanelScreen panel_screen;
} // namespace

verdite::SetupScreen *setup_panel_screen() { return &panel_screen; }

std::vector<std::string> setup_panel_take_controls() {
    std::lock_guard guard(shared.lock);
    return std::exchange(shared.controls, {});
}
} // namespace kf::ext

// On Android's UI thread, from SetupPanel.java.
extern "C" {
JNIEXPORT jstring JNICALL Java_io_github_voicedrew11_verdite1_SetupPanel_nativeSetupJson(JNIEnv *env, jclass) {
    std::lock_guard guard(kf::ext::shared.lock);
    return env->NewStringUTF(kf::ext::shared.view_json.c_str());
}

JNIEXPORT void JNICALL Java_io_github_voicedrew11_verdite1_SetupPanel_nativeSetupControl(JNIEnv *env, jclass, jstring line) {
    if (!line)
        return;
    const char *chars = env->GetStringUTFChars(line, nullptr);
    if (!chars)
        return;
    std::string copy(chars);
    env->ReleaseStringUTFChars(line, chars);
    std::lock_guard guard(kf::ext::shared.lock);
    kf::ext::shared.controls.push_back(std::move(copy));
}
}

#else

namespace kf::ext {
verdite::SetupScreen *setup_panel_screen() { return nullptr; }
std::vector<std::string> setup_panel_take_controls() { return {}; }
} // namespace kf::ext

#endif
