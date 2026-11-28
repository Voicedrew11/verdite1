// Telling the player about a new release (verdite/update_check.h): asked once a launch on a thread
// of its own, and shown once, at the title, never in play. A desktop draws a window here and a badge
// in the overlay's menu bar; Android shows a dialog from VerditeActivity.java. Notify only.
#include "internal.hpp"

#include <kf/platform/patch_archive.h>
#include <kf/platform/title.h>
#include <kf/platform/user_folder.h>

#include <verdite/update_check.h>

#include "menu_input.h"
#include "shell.h"

#include <verdite/ui.h>

#include <algorithm>
#include <array>
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
int notice_button = 0;   // the button with the focus
MenuInputs notice_inputs;
constexpr const char *notice_path = "notice.rml";

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

// The notice's buttons, in order, and what each does.
constexpr std::array<const char *, 3> button_names = {"download", "skip", "later"};

verdite::ui::Document *notice_document() {
    verdite::ui::Screen *screen = interface_screen();
    return screen ? screen->load(notice_path) : nullptr;
}

void show_notice() {
    verdite::ui::Document *page = notice_document();
    if (!page)
        return;
    notice_open = true;
    notice_button = 0;
    notice_inputs.reset();
    page->set_text("available", available_line());
    page->set_text("running", std::string("You have ") + KF_VERSION + ".");
    page->show();
    page->raise();
}

void close_notice() {
    notice_open = false;
    if (verdite::ui::Document *page = notice_document())
        page->hide();
}

void press_button(std::string_view name) {
    if (name == "download")
        open_page();
    else if (name == "skip")
        skip_release();
    else if (name != "later")
        return;
    close_notice();
}

void notice_input(verdite::MenuInput input) {
    using verdite::MenuInput;
    const int last = static_cast<int>(button_names.size()) - 1;
    switch (input) {
    case MenuInput::Left: case MenuInput::Up: notice_button = std::max(notice_button - 1, 0); break;
    case MenuInput::Right: case MenuInput::Down: notice_button = std::min(notice_button + 1, last); break;
    case MenuInput::Confirm: press_button(button_names[static_cast<std::size_t>(notice_button)]); break;
    case MenuInput::Back: press_button("later"); break;
    default: break;
    }
}
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
    std::thread(run_check, found, state_file(), forced ? verdite::UpdatePace::Now : verdite::UpdatePace::Hourly,
                std::string(url ? url : latest_release_url))
        .detach();
}

void update_notice_poll() {
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

bool update_notice_event(const SDL_Event &event) {
    if (!notice_open)
        return false;
    const bool pad = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP ||
                     event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION;
    if (!pad && event.type != SDL_EVENT_KEY_DOWN && event.type != SDL_EVENT_KEY_UP)
        return false;
    if (const std::optional<verdite::MenuInput> input = notice_inputs.take(event))
        notice_input(*input);
    return true;
}

void update_notice_action(const verdite::ui::Action &action) {
    if (action.kind != verdite::ui::ActionKind::Press)
        return;
    if (action.document == notice_path && notice_open) {
        press_button(action.value);
        return;
    }
    if (action.name == "update" && release)   // the F1 menu's badge
        show_notice();
}

void update_notice_frame() {
    if (!notice_open)
        return;
    if (const std::optional<verdite::MenuInput> input = notice_inputs.repeat())
        notice_input(*input);
    verdite::ui::Document *page = notice_document();
    if (!page || !notice_open)
        return;
    for (std::size_t i = 0; i < button_names.size(); ++i)
        page->set_classes(button_names[i], static_cast<int>(i) == notice_button ? "focused" : "");
}

std::string update_notice_badge_rml() {
    if (!release || badge_hidden)
        return {};
    return "<badge data-action=\"update\">Update available</badge>";
}

std::string update_notice_setting_note() { return release && !badge_hidden ? available_line() : std::string(); }
} // namespace kf::ext

