// Telling the player about a new release (verdite/update_check.h): asked once a launch on a thread
// of its own, and shown once, at the title, never in play. A desktop draws a window here and a badge
// in the overlay's menu bar; Android shows a dialog from VerditeActivity.java. Notify only.
#include "internal.hpp"

#include <kf/platform/patch_archive.h>
#include <kf/platform/title.h>
#include <kf/platform/user_folder.h>

#include <verdite/update_check.h>

#include <imgui.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#ifdef __ANDROID__
#include <SDL3/SDL_system.h>
#include <jni.h>
#endif

namespace kf::ext {
namespace {
constexpr const char *latest_release_url = "https://api.github.com/repos/Voicedrew11/verdite1/releases/latest";
constexpr const char *state_file_name = "update.json";

// The check's thread leaves what it found here. Shared, since the thread may outlive the game's exit.
struct Found {
    std::mutex lock;
    std::optional<verdite::UpdateRelease> release;
};

std::shared_ptr<Found> found;
std::optional<verdite::UpdateRelease> release;   // what the player is told about
bool badge_hidden = false;                       // until the next launch
bool notice_shown = false;                       // once a launch
bool notice_open = false;
bool notice_focus = false;

std::filesystem::path state_file() { return user_folder() / state_file_name; }

std::int64_t now_s() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

// On the update check's own thread.
void run_check(const std::shared_ptr<Found> &into, std::filesystem::path file, verdite::UpdatePace pace,
               std::string url) {
    auto fetch = [&url]() -> std::optional<std::string> {
        std::string error;
        const auto bytes = patch_archive_download(url.c_str(), &error);
        if (!bytes) {
            std::printf("[update] cannot reach the releases\n");
            return std::nullopt;
        }
        return std::string(bytes->begin(), bytes->end());
    };
    auto result = verdite::update_check_run({KF_VERSION, std::move(file), now_s(), pace, fetch});
    if (result)
        std::printf("[update] %s is available (running %s)\n", result->tag.c_str(), KF_VERSION);
    std::lock_guard guard(into->lock);
    into->release = std::move(result);
}

void skip_release() {
    if (release)
        verdite::update_check_skip(state_file(), release->tag);
    badge_hidden = true;
}

void open_page() {
    if (release && !SDL_OpenURL(release->page_url.c_str()))
        std::printf("[update] cannot open %s: %s\n", release->page_url.c_str(), SDL_GetError());
}

// The tag as a player reads a version.
std::string shown_version() {
    if (!release)
        return {};
    const std::string &tag = release->tag;
    return tag.starts_with('v') || tag.starts_with('V') ? tag.substr(1) : tag;
}

std::string available_line() { return std::string(product_name) + " " + shown_version() + " is available."; }

#ifdef __ANDROID__
std::mutex skip_lock;
bool skip_asked = false;   // from Android's UI thread

// Hands the release to the activity, whose dialog runs on the UI thread.
void show_notice() {
    auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity)
        return;
    jclass type = env->GetObjectClass(activity);
    const jmethodID method = env->GetMethodID(type, "showUpdateNotice",
                                              "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    jstring line = env->NewStringUTF(available_line().c_str());
    jstring running = env->NewStringUTF((std::string("You have ") + KF_VERSION + ".").c_str());
    jstring page = env->NewStringUTF(release->page_url.c_str());
    if (method && line && running && page)
        env->CallVoidMethod(activity, method, line, running, page);
    if (env->ExceptionCheck())
        env->ExceptionClear();
    env->DeleteLocalRef(page);
    env->DeleteLocalRef(running);
    env->DeleteLocalRef(line);
    env->DeleteLocalRef(type);
    env->DeleteLocalRef(activity);
}

void take_skip() {
    std::lock_guard guard(skip_lock);
    if (std::exchange(skip_asked, false))
        skip_release();
}
#else
void show_notice() {
    notice_open = true;
    notice_focus = true;
}

void take_skip() {}
#endif

// The gold of the update notice's main button and the badge, as Verdite2's and Verdite3's.
void push_gold() {
    const ImVec4 gold(0.83f, 0.69f, 0.22f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, gold);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.86f, 0.75f, 0.38f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.89f, 0.80f, 0.49f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.07f, 0.02f, 1.0f));
}

void pop_gold() { ImGui::PopStyleColor(4); }
} // namespace

void update_notice_start() {
    const char *env = std::getenv("KF_UPDATE_CHECK");
    if (env && std::strcmp(env, "0") == 0)
        return;
    const bool forced = env && std::strcmp(env, "force") == 0;
    if (!forced && !(KF_RELEASE && settings().update_check))
        return;
    if (user_folder().empty())
        return;
    const char *url = std::getenv("KF_TEST_UPDATE_URL");
    found = std::make_shared<Found>();
    std::thread(run_check, found, state_file(), forced ? verdite::UpdatePace::Now : verdite::UpdatePace::Daily,
                std::string(url ? url : latest_release_url))
        .detach();
}

void update_notice_poll() {
    take_skip();
    if (found && !release) {
        std::lock_guard guard(found->lock);
        release = std::move(found->release);
    }
    if (!release || notice_shown || !title_is_open() || overlay_open() || mods_pause_requested())
        return;
    notice_shown = true;
    std::printf("[update] telling the player about %s\n", release->tag.c_str());
    show_notice();
}

bool update_notice_open() { return notice_open; }

void update_notice_draw() {
    if (!notice_open || !release)
        return;
    const ImGuiViewport *screen = ImGui::GetMainViewport();
    const float width_px = ImGui::GetFontSize() * 26.0f;
    ImGui::SetNextWindowPos(screen->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width_px, 0.0f));
    if (std::exchange(notice_focus, false))
        ImGui::SetNextWindowFocus();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("Update available", nullptr, flags)) {
        ImGui::TextWrapped("%s", available_line().c_str());
        ImGui::TextDisabled("You have %s.", KF_VERSION);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        const float gap_px = ImGui::GetStyle().ItemSpacing.x;
        const ImVec2 button((ImGui::GetContentRegionAvail().x - gap_px * 2.0f) / 3.0f, 0.0f);
        push_gold();
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        const bool download = ImGui::Button("Download page", button);
        pop_gold();
        ImGui::SetItemTooltip("%s", release->page_url.c_str());
        ImGui::SameLine();
        const bool skip = ImGui::Button("Skip this version", button);
        ImGui::SameLine();
        const bool later = ImGui::Button("Later", button) || ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                           ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight);
        if (download)
            open_page();
        if (skip)
            skip_release();
        if (download || skip || later)
            notice_open = false;
    }
    ImGui::End();
}

float update_notice_badge_width_px() {
    if (!release || badge_hidden)
        return 0.0f;
    return ImGui::CalcTextSize("Update available!").x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

void update_notice_draw_badge() {
    if (!release || badge_hidden)
        return;
    constexpr const char *menu_id = "##update_badge";
    push_gold();
    if (ImGui::SmallButton("Update available!"))
        ImGui::OpenPopup(menu_id);
    pop_gold();
    if (!ImGui::BeginPopup(menu_id))
        return;
    ImGui::TextDisabled("%s %s (you have %s)", product_name, shown_version().c_str(), KF_VERSION);
    ImGui::Separator();
    if (ImGui::MenuItem("Open download page"))
        open_page();
    if (ImGui::MenuItem("Skip this version"))
        skip_release();
    if (ImGui::MenuItem("Hide until next launch"))
        badge_hidden = true;
    ImGui::EndPopup();
}

std::string update_notice_setting_note() { return release && !badge_hidden ? available_line() : std::string(); }
} // namespace kf::ext

#ifdef __ANDROID__
// On Android's UI thread, from the dialog in VerditeActivity.java.
extern "C" JNIEXPORT void JNICALL Java_io_github_voicedrew11_verdite1_VerditeActivity_nativeUpdateSkip(JNIEnv *, jclass) {
    std::lock_guard guard(kf::ext::skip_lock);
    kf::ext::skip_asked = true;
}
#endif
