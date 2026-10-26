// Android's settings panel: native Material views over the game, drawn by
// SettingsPanel.java from the option table's JSON. The panel lives on Android's
// UI thread; changes cross to the game thread through a queue and the view
// comes back as a string, both under one mutex. Elsewhere there is no panel.
#include "internal.hpp"
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifdef __ANDROID__
#include <jni.h>

namespace kf::ext {
namespace {
struct Change {
    std::string id, text;
};

// Shared with the UI thread.
struct Shared {
    std::mutex lock;
    bool open = false;
    bool overlay_asked = false;   // closed with "Mods and console"
    std::vector<Change> changes;
    std::string view_json;
};
Shared shared;
std::string sent_json;   // game thread: the view the panel was last told about

// Calls a no-argument method of the activity (VerditeActivity), which hands the work to its UI thread.
void call_activity(const char *method) {
    auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity) {
        std::printf("[settings] no activity for %s: %s\n", method, SDL_GetError());
        return;
    }
    jclass type = env->GetObjectClass(activity);
    jmethodID id = env->GetMethodID(type, method, "()V");
    if (id)
        env->CallVoidMethod(activity, id);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    env->DeleteLocalRef(type);
    env->DeleteLocalRef(activity);
}

// Publishes the view if it changed; true when it did.
bool publish_view() {
    std::string json = options_json();
    if (json == sent_json)
        return false;
    sent_json = json;
    std::lock_guard guard(shared.lock);
    shared.view_json = std::move(json);
    return true;
}

std::string java_string(JNIEnv *env, jstring text) {
    if (!text)
        return {};
    const char *chars = env->GetStringUTFChars(text, nullptr);
    if (!chars)
        return {};
    std::string copy(chars);
    env->ReleaseStringUTFChars(text, chars);
    return copy;
}
} // namespace

bool settings_panel_show() {
    publish_view();
    {
        std::lock_guard guard(shared.lock);
        shared.open = true;
    }
    call_activity("showSettingsPanel");
    return true;
}

bool settings_panel_open() {
    std::lock_guard guard(shared.lock);
    return shared.open;
}

void settings_panel_poll() {
    std::vector<Change> changes;
    bool open = false, overlay_asked = false;
    {
        std::lock_guard guard(shared.lock);
        changes.swap(shared.changes);
        open = shared.open;
        overlay_asked = std::exchange(shared.overlay_asked, false);
    }
    for (const Change &change : changes)
        if (const std::string why = option_change(change.id, change.text); !why.empty())
            std::printf("[settings] %s: %s\n", change.id.c_str(), why.c_str());
    if (overlay_asked)
        overlay_set_open(true);
    if (open && publish_view())
        call_activity("onOptionsChanged");
}
} // namespace kf::ext

// On Android's UI thread, from SettingsPanel.java.
extern "C" {
JNIEXPORT jstring JNICALL Java_io_github_voicedrew11_verdite1_SettingsPanel_nativeOptionsJson(JNIEnv *env, jclass) {
    std::lock_guard guard(kf::ext::shared.lock);
    return env->NewStringUTF(kf::ext::shared.view_json.c_str());
}

JNIEXPORT void JNICALL Java_io_github_voicedrew11_verdite1_SettingsPanel_nativeOptionChange(JNIEnv *env, jclass,
                                                                                             jstring id, jstring text) {
    kf::ext::Change change{kf::ext::java_string(env, id), kf::ext::java_string(env, text)};
    std::lock_guard guard(kf::ext::shared.lock);
    kf::ext::shared.changes.push_back(std::move(change));
}

JNIEXPORT void JNICALL Java_io_github_voicedrew11_verdite1_SettingsPanel_nativeClosed(JNIEnv *, jclass,
                                                                                       jboolean open_overlay) {
    std::lock_guard guard(kf::ext::shared.lock);
    kf::ext::shared.open = false;
    kf::ext::shared.overlay_asked = open_overlay == JNI_TRUE;
}
}

#else

namespace kf::ext {
bool settings_panel_show() { return false; }
bool settings_panel_open() { return false; }
void settings_panel_poll() {}
} // namespace kf::ext

#endif
