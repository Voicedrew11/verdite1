#include <kf/platform/crash.h>
#include <kf/platform/host.h>
#include <kf/platform/input.h>
#include <kf/platform/language.h>
#include <kf/platform/user_folder.h>
#include <kf/game/actor.h>
#include <kf/game/effect.h>
#include <kf/game/player.h>
#if KF_EXT
#include <kf/ext/ext.h>
#endif
#include <verdite/crash.h>
#include <SDL3/SDL.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "build_info.h"

#ifdef __ANDROID__
#include <jni.h>
#endif

#ifndef _WIN32
extern char **environ;
#endif

namespace kf {
namespace {
constexpr std::uint64_t ns_per_ms = 1000000;

struct ActionName {
    KfActorAction action;
    const char *name;
};
constexpr std::array<ActionName, 18> action_names = {
    {{KF_ACTOR_ACTION_IDLE, "idle"},
     {KF_ACTOR_ACTION_WANDER, "wander"},
     {KF_ACTOR_ACTION_PURSUE, "pursue"},
     {KF_ACTOR_ACTION_RETREAT, "retreat"},
     {KF_ACTOR_ACTION_MELEE_ATTACK, "melee attack"},
     {KF_ACTOR_ACTION_HIT_REACTION, "hit reaction"},
     {KF_ACTOR_ACTION_DYING, "dying"},
     {KF_ACTOR_ACTION_JUMP_ATTACK, "jump attack"},
     {KF_ACTOR_ACTION_SPECIAL_ATTACK, "special attack"},
     {KF_ACTOR_ACTION_DRIFT, "drift"},
     {KF_ACTOR_ACTION_EFFECT0, "effect 0"},
     {KF_ACTOR_ACTION_EFFECT1, "effect 1"},
     {KF_ACTOR_ACTION_EFFECT2, "effect 2"},
     {KF_ACTOR_ACTION_MULTI_HIT_ATTACK, "multi-hit attack"},
     {KF_ACTOR_ACTION_EXIT_BLOCKED_PLACEMENT, "exit blocked placement"},
     {KF_ACTOR_ACTION_RETURN_HOME, "return home"},
     {KF_ACTOR_ACTION_POST_DEATH, "post death"},
     {KF_ACTOR_ACTION_NONE, "none"}}};

struct Watched {
    InputContext context = InputContext::Opening;
    int floor = -1;
    bool alive = false;
};
Watched watched;

std::int64_t number(auto value) {
    return static_cast<std::int64_t>(value);
}

// The index of pointer in records, if it points at one; compared as addresses, so a damaged
// pointer is never followed.
template <typename Record, std::size_t count>
std::optional<std::size_t> index_in(const std::array<Record, count> &records,
                                    const Record *pointer) {
    const auto first = reinterpret_cast<std::uintptr_t>(records.data());
    const auto at = reinterpret_cast<std::uintptr_t>(pointer);
    if (at < first || at >= first + sizeof(Record) * count || (at - first) % sizeof(Record) != 0)
        return std::nullopt;
    return (at - first) / sizeof(Record);
}

void write_game(verdite::CrashWriter &out) {
    out.line("context", input_context_name(host_input_context()));
    out.line("language", language_name(game_language()));
    out.line("frames presented", number(host_frames_presented()));
    out.line("clock ms", number(host_clock_ns() / ns_per_ms));
}

void write_player(verdite::CrashWriter &out) {
    const auto &vitals = player_state.vitals;
    out.line("hp", number(vitals.current_hp));
    out.line("max hp", number(vitals.maximum_hp));
    out.line("mp", number(vitals.current_mp));
    out.line("max mp", number(vitals.maximum_mp));
    out.line("level", number(player_state.progress_state.level));
    out.line("gold", number(player_state.gold));
    out.line("floor", number(player_state.progress_state.current_floor));
    const auto &position = player_state.camera_position;
    out.triple("position", position.vx, position.vy, position.vz);
    const auto &rotation = player_state.camera_rotation;
    out.triple("rotation", rotation.vx, rotation.vy, rotation.vz);
}

void write_action(verdite::CrashWriter &out, KfActorAction action) {
    for (const ActionName &entry : action_names)
        if (entry.action == action) {
            out.line("action", entry.name);
            return;
        }
    out.line("action", number(action));
}

// actor_state.current is set only while the actor loop updates that actor.
void write_actor(verdite::CrashWriter &out) {
    if (!actor_state.current) {
        out.text("(none: no actor was being updated)\n");
        return;
    }
    const auto index = index_in(actor_state.actors, actor_state.current);
    if (!index) {
        out.hex("damaged pointer", reinterpret_cast<std::uintptr_t>(actor_state.current));
        return;
    }
    const KfActor &actor = actor_state.actors[*index];
    out.line("slot", number(*index));
    out.line("definition", number(actor.definition_id));
    out.line("slot state", number(actor.slot_state));
    out.line("lifecycle", number(actor.lifecycle));
    write_action(out, actor.action);
    out.line("action progress", number(actor.action_progress));
    out.line("health", number(actor.health));
    out.triple("position", actor.position.vx, actor.position.vy, actor.position.vz);
    out.triple("rotation", actor.rotation.vx, actor.rotation.vy, actor.rotation.vz);
    out.line("cell x", number(actor.cell_x));
    out.line("cell z", number(actor.cell_z));
    out.line("animation clip", number(actor.animation_clip));
    out.line("animation phase", number(actor.animation_phase));
    out.line("vertical state", number(actor.vertical_state));
}

// effect_state.current_record is the last effect updated, possibly in an earlier update.
void write_effect(verdite::CrashWriter &out) {
    if (!effect_state.current_record) {
        out.text("(none yet)\n");
        return;
    }
    const auto index = index_in(effect_state.records, effect_state.current_record);
    if (!index) {
        out.hex("damaged pointer", reinterpret_cast<std::uintptr_t>(effect_state.current_record));
        return;
    }
    const KfEffectRecord &effect = effect_state.records[*index];
    out.line("slot", number(*index));
    out.hex("type", number(effect.type));
    out.line("kind", number(effect.kind));
    out.line("id", number(effect.id));
    out.line("phase", number(effect.phase));
    out.line("animation phase", number(effect.animation_phase));
    out.line("frames remaining", number(effect.frames_remaining));
    out.triple("position", effect.position.vx, effect.position.vy, effect.position.vz);
}

// The variables that change how the game runs: what the player's own settings do not show.
void write_environment(verdite::CrashWriter &out) {
#ifdef _WIN32
    char **variables = _environ;
#else
    char **variables = environ;
#endif
    for (; variables && *variables; ++variables) {
        const std::string_view variable(*variables);
        if (!variable.starts_with("KF_"))
            continue;
        out.text(variable);
        out.text("\n");
    }
}

std::string reports_directory() {
    const auto &folder = user_folder();
    const auto directory = (folder.empty() ? std::filesystem::path("crashes") : folder / "crashes").u8string();
    return {directory.begin(), directory.end()};
}

void note(const std::string &text) {
    verdite::crash_event(text);
}

// Through a volatile pointer, so neither the compiler nor the analyzer sees the null.
[[gnu::noinline]] void fault_on_purpose() {
    int *volatile target = nullptr;
    *target = 1;
}

// Through a volatile, so the compiler cannot call the loop undefined and drop it.
[[gnu::noinline]] void hang_on_purpose() {
    for (volatile int forever = 0; forever == 0;) {
    }
}

// What the player is told, by what the report is of and when.
struct Telling {
    const char *title;
    const char *body;
};
constexpr Telling telling_crash_later = {"The game stopped last time",
                                         "The game ran into an error it could not recover from and stopped last time."};
constexpr Telling telling_hang_later = {"The game stopped responding last time",
                                        "The game stopped responding last time, and had to be closed."};
constexpr const char *report_saved =
    "A report was saved. Sending it, along with what you were doing, helps get this fixed:";

Telling telling_later(const std::string &folder) {
    return verdite::crash_report_kind(folder) == verdite::CrashReportKind::Hang ? telling_hang_later
                                                                                : telling_crash_later;
}

#ifdef __ANDROID__
// Hands the report to the activity, whose dialog runs on the UI thread: a phone has no folder a
// player can open, so it offers to share the report instead.
void tell(const Telling &telling, const std::string &folder) {
    auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity)
        return;
    jclass type = env->GetObjectClass(activity);
    const jmethodID method = env->GetMethodID(type, "showCrashNotice",
                                              "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    jstring title = env->NewStringUTF(telling.title);
    jstring body = env->NewStringUTF((std::string(telling.body) + "\n\n" + report_saved).c_str());
    jstring path = env->NewStringUTF(folder.c_str());
    if (method && title && body && path)
        env->CallVoidMethod(activity, method, title, body, path);
    if (env->ExceptionCheck())
        env->ExceptionClear();
    env->DeleteLocalRef(path);
    env->DeleteLocalRef(body);
    env->DeleteLocalRef(title);
    env->DeleteLocalRef(type);
    env->DeleteLocalRef(activity);
}
#else
constexpr Telling telling_now = {"The game stopped",
                                 "The game ran into an error it could not recover from and has stopped."};

std::string telling_text(const Telling &telling, const std::string &folder) {
    return std::string(telling.body) + "\n\n" + report_saved + "\n\n" + folder;
}

// Blocks until the player closes it. False if no box could be shown, so they are told next time.
bool tell(const Telling &telling, const std::string &folder, const char *close_label) {
    enum Button { Close, OpenFolder };
    const std::array<SDL_MessageBoxButtonData, 2> buttons = {
        {{0, OpenFolder, "Open the report's folder"}, {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, Close, close_label}}};
    const std::string text = telling_text(telling, folder);
    const SDL_MessageBoxData box{SDL_MESSAGEBOX_ERROR, nullptr, telling.title, text.c_str(),
                                 static_cast<int>(buttons.size()), buttons.data(), nullptr};
    for (;;) {
        int pressed = Close;
        if (!SDL_ShowMessageBox(&box, &pressed)) {
            std::fprintf(stderr, "[crash] cannot show the crash window: %s\n", SDL_GetError());
            return false;
        }
        if (pressed != OpenFolder)
            return true;
        const std::string url = verdite::crash_file_url(folder);
        if (!SDL_OpenURL(url.c_str()))
            std::fprintf(stderr, "[crash] cannot open %s: %s\n", url.c_str(), SDL_GetError());
    }
}
#endif

// KF_TEST_CRASH=fault|abort|soft|hang@<gameplay polls>: one crash on purpose, for automated tests.
void test_crash() {
    static const char *spec = std::getenv("KF_TEST_CRASH");
    static long polls = 0;
    if (!spec || host_input_context() != InputContext::Gameplay)
        return;
    const char *at = std::strchr(spec, '@');
    if (++polls != (at ? std::atol(at + 1) : 1))
        return;
    const std::string_view kind(spec, at ? static_cast<std::size_t>(at - spec) : std::strlen(spec));
    if (kind == "fault")
        fault_on_purpose();
    else if (kind == "abort")
        std::abort();
    else if (kind == "soft")
        verdite::crash_report_soft("KF_TEST_CRASH");
    else if (kind == "hang")
        hang_on_purpose();
}

// On a callback SDL runs where the event is raised: on a phone it stops the game's thread inside
// its event pump as the app goes to the background, before the game sees the event.
bool SDLCALL watch_background(void *, SDL_Event *event) {
    static bool in_background = false;   // a first foreground event may come with no background before it
    if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND && !in_background) {
        in_background = true;
        verdite::crash_hang_pause();
    } else if (event->type == SDL_EVENT_DID_ENTER_FOREGROUND && in_background) {
        in_background = false;
        verdite::crash_hang_resume();
    }
    return true;
}
} // namespace

void crash_start() {
    verdite::CrashConfig config;
    config.tag = "KF1";
    config.directory = reports_directory();
    config.build = std::string(KF_VERSION " ") + KF_BUILD_COMMIT + " " + KF_BUILD_TYPE;
    config.sections = {{"game", write_game},
                       {"player", write_player},
                       {"actor being updated", write_actor},
                       {"last effect updated", write_effect}};
#if KF_EXT
    config.sections.push_back({"settings", ext::crash_write_settings});
#endif
    config.sections.push_back({"environment", write_environment});
    if (!verdite::crash_install(std::move(config))) {
        std::fprintf(stderr, "[crash] crash reports are off\n");
        return;
    }
    // A copy of the log in each report: the next launch starts kf.log again.
    const auto log = (user_folder() / "kf.log").u8string();
    verdite::crash_keep_file({reinterpret_cast<const char *>(log.data()), log.size()});
}

std::optional<int> crash_window_run(int argc, char **argv) {
    const auto request = verdite::crash_window_request(argc, argv);
    if (!request)
        return std::nullopt;
#ifndef __ANDROID__
    if (tell(telling_now, request->folder, "Quit"))
        verdite::crash_report_seen(request->folder);
#endif
    return request->exit_code;
}

void crash_tell_player() {
    const char *window = std::getenv("KF_CRASH_WINDOW");
    if (window && std::strcmp(window, "0") == 0)
        return;
    verdite::crash_window_enable();
    const auto folder = verdite::crash_take_unseen_report();
    if (!folder)
        return;
#ifdef __ANDROID__
    tell(telling_later(*folder), *folder);
#else
    tell(telling_later(*folder), *folder, "Continue");
#endif
}

void crash_watch_hangs(int hang_s) {
    if (const char *seconds = std::getenv("KF_HANG"))
        hang_s = std::atoi(seconds);
    constexpr int ms_per_s = 1000;
    if (hang_s > 0 && verdite::crash_hang_watch_start(hang_s * ms_per_s) && !SDL_AddEventWatch(watch_background, nullptr))
        std::fprintf(stderr, "[crash] cannot watch for the app going to the background: %s\n", SDL_GetError());
}

void crash_poll() {
    verdite::crash_hang_beat();
    const InputContext context = host_input_context();
    if (context != watched.context) {
        watched.context = context;
        note(std::string("context: ") + input_context_name(context));
    }
    if (context == InputContext::Opening || player_state.vitals.maximum_hp == 0)
        return;
    const int floor = static_cast<int>(player_state.progress_state.current_floor);
    if (floor != watched.floor) {
        watched.floor = floor;
        note("floor " + std::to_string(floor));
    }
    const bool alive = player_state.vitals.current_hp != 0;
    if (alive != watched.alive) {
        watched.alive = alive;
        if (!alive)
            note("the player died");
    }
    test_crash();
}
} // namespace kf
