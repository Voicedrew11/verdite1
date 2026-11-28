// F1 overlay: menu bar with Settings, Mods and Console windows, drawn with Dear
// ImGui over the game's frame. The game pauses while it is open (configurable).
#include "build_info.h"
#include "internal.hpp"
#include "menu.h"
#include "shell.h"
#include <verdite/ui.h>
#include <backends/imgui_impl_sdl3.h>
#include <imgui.h>
#include <imgui_internal.h>   // FindRenderedTextEnd
#include <kf/platform/host.h>
#include <kf/platform/language_runtime.h>
#include <kf/platform/user_folder.h>
#include <kf/renderer/frontend.h>
#include <kf/renderer/renderer.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace kf::ext {
namespace {
SDL_Window *main_window;
bool running;
bool imgui;   // false when headless
bool open;
int fps_frames;
Uint64 fps_started;
float fps_value;
int last_context;
float applied_scale = 1.0f;   // layout uses this, never the slider's live value

void load_font() {
    ImGuiIO &io = ImGui::GetIO();
    const char *candidates[] = {"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf",
                                "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
                                "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf",
                                "/system/fonts/Roboto-Regular.ttf"};
    if (!settings().ui_font.empty() && std::filesystem::exists(settings().ui_font)) {
        io.Fonts->AddFontFromFileTTF(settings().ui_font.c_str(), 18.0f);
        return;
    }
    for (const char *path : candidates)
        if (std::filesystem::exists(path)) {
            io.Fonts->AddFontFromFileTTF(path, 18.0f);
            return;
        }
    io.Fonts->AddFontDefault();
}

void apply_style() {
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.WindowBorderSize = 1.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.94f;
    applied_scale = settings().ui_scale;
    style.ScaleAllSizes(applied_scale);
    style.FontScaleMain = applied_scale;
}

// Splits the item width between the slider and a number box, then draws the label.
template <typename T, typename Slider, typename Input>
bool slider_with_box(const char *label, T *value, T min, T max, float snap, bool *committed, Slider slider, Input input) {
    const ImGuiStyle &style = ImGui::GetStyle();
    const float total = ImGui::CalcItemWidth();
    const float box = ImGui::CalcTextSize("00000.00").x + style.FramePadding.x * 2.0f;
    ImGui::PushID(label);
    ImGui::BeginGroup();
    ImGui::SetNextItemWidth(std::max(total - box - style.ItemInnerSpacing.x, 40.0f));
    bool changed = slider(value);
    if (changed && snap > 0)
        *value = static_cast<T>(std::round(static_cast<double>(*value) / snap) * snap);
    bool done = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SameLine(0, style.ItemInnerSpacing.x);
    ImGui::SetNextItemWidth(box);
    T typed = *value;
    if (input(&typed)) {
        *value = std::clamp(typed, min, max);
        changed = true;
    }
    done |= ImGui::IsItemDeactivatedAfterEdit();
    const char *end = ImGui::FindRenderedTextEnd(label);
    if (end != label) {
        ImGui::SameLine(0, style.ItemInnerSpacing.x);
        ImGui::TextUnformatted(label, end);
    }
    ImGui::EndGroup();
    ImGui::PopID();
    if (committed)
        *committed = done;
    return changed;
}

void set_open(bool value) {
    if (open == value)
        return;
    open = value;
    if (open)
        SDL_SetWindowRelativeMouseMode(main_window, false);
    else
        binding_capture_cancel();
}

bool is_input_event(Uint32 type) {
    switch (type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT: case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
        return true;
    default:
        return false;
    }
}
}

bool slider_input(const char *label, float *value, float min, float max, const char *format, float snap, bool *committed) {
    return slider_with_box(label, value, min, max, snap, committed,
        [&](float *v) { return ImGui::SliderFloat("##slider", v, min, max, format, ImGuiSliderFlags_AlwaysClamp); },
        [&](float *v) { return ImGui::InputFloat("##value", v, 0.0f, 0.0f, format); });
}

bool slider_input(const char *label, int *value, int min, int max, const char *format, bool *committed) {
    return slider_with_box(label, value, min, max, 0.0f, committed,
        [&](int *v) { return ImGui::SliderInt("##slider", v, min, max, format, ImGuiSliderFlags_AlwaysClamp); },
        [&](int *v) { return ImGui::InputInt("##value", v, 0, 0); });
}

SDL_Window *window() { return main_window; }

void overlay_set_open(bool value) { set_open(value); }

void overlay_restyle() {
    if (!imgui)
        return;
    ImGui::GetStyle() = ImGuiStyle();
    apply_style();
}

// The overlay's windows or Android's settings panel cover the game.
static bool covered() {
    return open || menu_open() || setup_open() || update_notice_open();
}

bool overlay_open() { return covered(); }
bool overlay_captures_input() { return covered(); }
// A text field on a mod's page that has the keyboard takes the keys too, but not the mouse.
bool overlay_captures_keys() {
    const verdite::ui::Screen *screen = interface_screen();
    return covered() || (screen && screen->wants_text());
}
bool pause_requested() {
    const bool shown = open || menu_open();
    return setup_open() || update_notice_open() || (shown && settings().pause_when_open) || mods_pause_requested();
}
bool pause_keeps_music() {
    if ((open || menu_open()) && settings().pause_when_open)
        return false;
    return mods_pause_keeps_music();
}
// A paused mod still gets on_draw, which is how it unpauses.
bool redraw_while_paused() {
    return open || menu_open() || setup_shows_game() || update_notice_open() || mods_pause_requested();
}
bool wants_cursor() { return covered() || mods_take_mouse(); }

ViewRect game_view() {
    int width = 0, height = 0;
    if (main_window)
        SDL_GetWindowSizeInPixels(main_window, &width, &height);
    const auto rect = renderer_present_rect(width, height);
    const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
    return {static_cast<float>(rect.x) / scale.x, static_cast<float>(rect.y) / scale.y,
            static_cast<float>(rect.width) / scale.x, static_cast<float>(rect.height) / scale.y};
}

ViewRect game_view_px() {
    int width = 0, height = 0;
    if (main_window)
        SDL_GetWindowSizeInPixels(main_window, &width, &height);
    const auto rect = renderer_present_rect(width, height);
    return {static_cast<float>(rect.x), static_cast<float>(rect.y), static_cast<float>(rect.width),
            static_cast<float>(rect.height)};
}

// The original 320 columns within a whole picture.
static ViewRect original_within(const ViewRect &whole) {
    const int extra = renderer_extra_columns();
    const float column = whole.w / static_cast<float>(render_width + 2 * extra);
    return {whole.x + column * static_cast<float>(extra), whole.y,
            column * static_cast<float>(render_width), whole.h};
}

ViewRect original_view() { return original_within(game_view()); }
ViewRect original_view_px() { return original_within(game_view_px()); }

bool start(SDL_Window *handle, Overlay overlay) {
    main_window = handle;
    settings_load();
    // Headless: settings and mods, no Dear ImGui (nothing draws it).
    imgui = overlay == Overlay::Drawn;
    if (!imgui) {
        running = true;
        apply_audio_settings();
        apply_control_settings();
        apply_render_settings();
        mods_start();
        online_start();   // two headless instances can meet in a room
        setup_begin(overlay);
        return true;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    static std::string ini_path = (user_dir() / "overlay.ini").string();
    io.IniFilename = ini_path.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    load_font();
    apply_style();
    if (!ImGui_ImplSDL3_InitForVulkan(handle) || !renderer_overlay_start()) {
        std::printf("[overlay] cannot initialize Dear ImGui\n");
        return false;
    }
    interface_start(handle);
    running = true;
    apply_display_settings();
    apply_audio_settings();
    apply_control_settings();
    apply_render_settings();
    std::printf("[overlay] ready - press F1 for settings, mods and console. User folder: %s\n",
                user_dir().string().c_str());
    std::printf("[overlay] %s %s (%s)\n", product_name, KF_VERSION, KF_BUILD_COMMIT);
    update_notice_start();
    mods_start();
    discord_start();
    online_start();
    setup_begin(overlay);
    fps_started = SDL_GetTicks();
    return true;
}

void shutdown() {
    if (!running)
        return;
    mods_shutdown();
    discord_stop();
    online_stop();
    settings_page_abandon();
    settings_save();
    running = false;
    if (!imgui)
        return;
    interface_stop();
    renderer_overlay_stop();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

bool process_event(const SDL_Event &event) {
    // Android may end the process in the background without an exit; the game sees the event too.
    if (event.type == SDL_EVENT_WILL_ENTER_BACKGROUND && running && !settings_page_holding())
        settings_save();
    if (event.type == SDL_EVENT_WILL_ENTER_BACKGROUND)
        online_went_to_background();
    if (!running || !imgui)
        return false;
    if (binding_capture_event(event))
        return true;
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
        const bool alt_enter = event.key.key == SDLK_RETURN && (event.key.mod & SDL_KMOD_ALT);
        if (event.key.key == SDLK_F11 || alt_enter) {
            settings().fullscreen = !settings().fullscreen;
            apply_display_settings();
            settings_mark_dirty();
            return true;
        }
        if (event.key.key == SDLK_F1) {
            menu_set_open(!menu_open());
            return true;
        }
    }
    if (event.type == SDL_EVENT_KEY_UP && (event.key.key == SDLK_F1 || event.key.key == SDLK_F11))
        return true;
    if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
        apply_render_settings();
    // The interface is drawn over Dear ImGui, so it is asked first.
    if (interface_event(event) || update_notice_event(event) || menu_event(event) || setup_event(event))
        return true;
    ImGui_ImplSDL3_ProcessEvent(&event);
    if (event.type == SDL_EVENT_MOUSE_WHEEL)
        mods_ui_wheel(event.wheel.y);
    // A mod that takes the mouse (the full map: zoom, drag, hover) gets it; the keyboard still reaches the game.
    const bool mouse_event = event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
        || event.type == SDL_EVENT_MOUSE_BUTTON_UP || event.type == SDL_EVENT_MOUSE_WHEEL;
    if (mods_take_mouse() && mouse_event)
        return true;
    if (covered())
        return is_input_event(event.type);
    // The keys typed into a mod's text box are not the game's. A key let go still reaches the game,
    // so a key pressed before the box had the keyboard is not left held.
    const bool typed = event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_TEXT_INPUT ||
                       event.type == SDL_EVENT_TEXT_EDITING;
    const verdite::ui::Screen *screen = interface_screen();
    return typed && screen && screen->wants_text();
}

void poll() {
    if (!running)
        return;
    mods_poll();
    options_poll();
    update_notice_poll();
    online_poll();
    messages_poll();
    if (imgui) {
        discord_poll();
        touch_commands_poll();
    }
    settings_save_if_dirty();
}

void game_frame(int input_context) {
    if (!running)
        return;
    last_context = input_context;
    mods_frame(input_context);
}

static std::string message_text;
static Uint64 message_started;

void show_message(const std::string &text) {
    message_text = text;
    message_started = SDL_GetTicks();
}

// The message over the game: centred, fading out at the end.
static void draw_message(verdite::ui::Canvas &canvas, int width_px, int height_px) {
    constexpr Uint64 shown_ms = 1800, fade_ms = 400;
    const Uint64 age = SDL_GetTicks() - message_started;
    if (message_text.empty() || age >= shown_ms)
        return;
    const float alpha = age + fade_ms > shown_ms ? static_cast<float>(shown_ms - age) / fade_ms : 1.0f;
    const float size = std::max(18.0f, static_cast<float>(height_px) / 30.0f);
    const verdite::ui::Point extent = canvas.text_size(message_text, size);
    const verdite::ui::Point at{(static_cast<float>(width_px) - extent.x) * 0.5f, static_cast<float>(height_px) * 0.2f};
    canvas.rounded_rect({at.x - size * 0.6f, at.y - size * 0.3f, extent.x + size * 1.2f, extent.y + size * 0.6f},
                        size * 0.3f, {0, 0, 0, 150 / 255.0f * alpha});
    canvas.text(at, message_text, {1, 235 / 255.0f, 170 / 255.0f, alpha}, size);
}

static void count_frame() {
    ++fps_frames;
    const Uint64 now = SDL_GetTicks();
    if (now - fps_started >= 1000) {
        fps_value = static_cast<float>(fps_frames) * 1000.0f / static_cast<float>(now - fps_started);
        fps_frames = 0;
        fps_started = now;
    }
}

// The frame counter in the top left, below the menu bar while it shows.
static void draw_fps(verdite::ui::Canvas &canvas, float px_per_dp) {
    char text[32];
    std::snprintf(text, sizeof text, "%.0f fps", fps_value);
    canvas.text({8 * px_per_dp, (open ? 30.0f : 6.0f) * px_per_dp}, text, {1, 1, 120 / 255.0f, 1},
                hud_font_size * px_per_dp, verdite::ui::Shadow::Drop);
}

const verdite::ui::Frame *build_interface(int width_px, int height_px) {
    verdite::ui::Screen *screen = running ? interface_screen() : nullptr;
    if (!screen || width_px <= 0 || height_px <= 0)
        return nullptr;
    count_frame();
    // KF_TEST_SETTINGS=<page>: opens the menu on that page, for captures.
    static const char *test_page = std::getenv("KF_TEST_SETTINGS");
    if (test_page) {
        menu_open_page(test_page);
        test_page = nullptr;
        // KF_TEST_LANGUAGE_TASK=download: presses "Download English patch".
        if (const char *task = std::getenv("KF_TEST_LANGUAGE_TASK"); task && std::strcmp(task, "download") == 0)
            language_task_download_english();
    }
    while (std::optional<verdite::ui::Action> action = screen->take_action()) {
        menu_action(*action);
        setup_action(*action);
        update_notice_action(*action);
        mods_ui_action(*action);
    }
    update_notice_frame();
    setup_frame();
    menu_frame();
    verdite::ui::Canvas &front = screen->canvas(verdite::ui::Layer::Front);
    item_description_draw(screen->canvas(verdite::ui::Layer::Back));
    mods_draw_hud();
    touch_controls_draw(front);
    draw_message(front, width_px, height_px);
    if (settings().show_fps)
        draw_fps(front, screen->px_per_dp());
    return interface_frame(width_px, height_px);
}

ImDrawData *build_overlay(int width, int height) {
    if (!running || !imgui || width <= 0 || height <= 0)
        return nullptr;
    // The Vulkan renderer's NewFrame only checks that it was started, so it is not called.
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGui::Render();
    return ImGui::GetDrawData();
}
}
