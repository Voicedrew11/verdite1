// F1 overlay: menu bar with Settings, Mods and Console windows, drawn with Dear
// ImGui over the game's frame. The game pauses while it is open (configurable).
#include "internal.hpp"
#include <backends/imgui_impl_sdl3.h>
#include <imgui.h>
#include <imgui_internal.h>   // FindRenderedTextEnd
#include <kf/platform/host.h>
#include <kf/platform/language_runtime.h>
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
bool show_settings, show_mods = true, show_console, show_about;
bool console_autoscroll = true;
char console_filter[128];
std::vector<ConsoleLine> console_lines;
std::uint64_t console_serial;
int fps_frames;
Uint64 fps_started;
float fps_value;
int last_context;
float applied_scale = 1.0f;   // layout uses this, never the slider's live value
std::string settings_page = "Display";

void load_font() {
    ImGuiIO &io = ImGui::GetIO();
    const char *candidates[] = {"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf",
                                "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/system/fonts/Roboto-Regular.ttf"};
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
}

// ---- Settings: the option table, drawn as it comes ----

// A number with settle set is held here while its slider is dragged.
std::string settling_id;
float settling_value;

void show_help(const verdite::OptionRow &row) {
    if (!row.help.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", row.help.c_str());
}

void draw_choice(const verdite::OptionRowView &view) {
    const verdite::OptionRow &row = *view.row;
    const auto current = std::ranges::find(row.choices, view.value, &verdite::OptionChoice::value);
    const char *preview = current != row.choices.end() ? current->label.c_str() : "";
    if (!ImGui::BeginCombo(row.label.c_str(), preview))
        return;
    for (const auto &choice : row.choices)
        if (ImGui::Selectable(choice.label.c_str(), choice.value == view.value))
            options().set(row.id, choice.value);
    ImGui::EndCombo();
}

void draw_number(const verdite::OptionRowView &view) {
    const verdite::OptionRow &row = *view.row;
    const bool settling = settling_id == row.id;
    float value = settling ? settling_value : static_cast<float>(std::get<double>(view.value));
    bool committed = false;
    const bool moved = slider_input(row.label.c_str(), &value, static_cast<float>(row.min), static_cast<float>(row.max),
                                    row.format.empty() ? "%.2f" : row.format.c_str(), static_cast<float>(row.step),
                                    &committed);
    if (!row.settle) {
        if (moved)
            options().set(row.id, static_cast<double>(value));
        return;
    }
    if (moved) {
        settling_id = row.id;
        settling_value = value;
    }
    if (committed) {
        options().set(row.id, static_cast<double>(value));
        settling_id.clear();
    }
}

void draw_status(const verdite::OptionRowView &view) {
    const std::string &text = std::get<std::string>(view.value);
    if (view.row->label.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", text.c_str());
        ImGui::PopStyleColor();
        return;
    }
    ImGui::TextUnformatted(view.row->label.c_str());
    ImGui::SameLine(180.0f * applied_scale);
    ImGui::TextDisabled("%s", text.c_str());
}

void draw_option(const verdite::OptionRowView &view) {
    const verdite::OptionRow &row = *view.row;
    ImGui::PushID(row.id.c_str());
    ImGui::BeginDisabled(!view.state.enabled);
    switch (row.kind) {
    case verdite::OptionKind::Toggle: {
        bool on = std::get<bool>(view.value);
        if (ImGui::Checkbox(row.label.c_str(), &on))
            options().set(row.id, on);
        break;
    }
    case verdite::OptionKind::Choice:
        draw_choice(view);
        break;
    case verdite::OptionKind::Number:
        draw_number(view);
        break;
    case verdite::OptionKind::Action:
        if (ImGui::Button(row.label.c_str()))
            options().run(row.id);
        break;
    case verdite::OptionKind::Status:
        draw_status(view);
        break;
    }
    show_help(row);
    ImGui::EndDisabled();
    if (!view.state.note.empty()) {
        ImGui::Indent();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", view.state.note.c_str());
        ImGui::PopStyleColor();
        ImGui::Unindent();
    }
    ImGui::PopID();
}

void draw_option_page(const verdite::OptionPageView &page) {
    for (const auto &section : page.sections) {
        if (!section.title.empty())
            ImGui::SeparatorText(std::string(section.title).c_str());
        for (const auto &row : section.rows)
            draw_option(row);
    }
}

void draw_settings_window() {
    ImGui::SetNextWindowSize(ImVec2(560, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Settings", &show_settings)) {
        ImGui::End();
        return;
    }
    const auto pages = options().view(platform_features());
    const auto shown = std::ranges::find(pages, std::string_view(settings_page), &verdite::OptionPageView::title);
    const verdite::OptionPageView *page = shown != pages.end() ? &*shown : pages.empty() ? nullptr : &pages.front();
    ImGui::BeginChild("pages", ImVec2(130 * applied_scale, 0), ImGuiChildFlags_Borders);
    for (const auto &each : pages)
        if (ImGui::Selectable(std::string(each.title).c_str(), &each == page))
            settings_page = each.title;
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("page");
    if (page)
        draw_option_page(*page);
    ImGui::EndChild();
    ImGui::End();
}

void draw_console_window() {
    ImGui::SetNextWindowSize(ImVec2(760, 360), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Console", &show_console)) {
        ImGui::End();
        return;
    }
    console_serial = console_copy(console_serial, console_lines);
    if (console_lines.size() > 8000)
        console_lines.erase(console_lines.begin(), console_lines.end() - 8000);
    if (ImGui::Button("Clear")) {
        console_lines.clear();
        console_clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy")) {
        std::string all;
        for (const auto &line : console_lines)
            all += line.text + "\n";
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &console_autoscroll);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##filter", "filter", console_filter, sizeof console_filter);
    ImGui::SameLine();
    ImGui::TextDisabled("log: %s", (user_dir() / "kf.log").string().c_str());
    ImGui::Separator();
    ImGui::BeginChild("lines", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto &line : console_lines) {
        if (console_filter[0] && line.text.find(console_filter) == std::string::npos)
            continue;
        const bool error = line.text.find("error") != std::string::npos || line.text.find("Error") != std::string::npos;
        const bool mods = line.text.rfind("[mods]", 0) == 0;
        if (error)
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", line.text.c_str());
        else if (mods)
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s", line.text.c_str());
        else
            ImGui::TextUnformatted(line.text.c_str());
    }
    if (console_autoscroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::End();
}

void draw_mods_window() {
    ImGui::SetNextWindowSize(ImVec2(620, 460), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Mods", &show_mods))
        mods_draw_panel();
    ImGui::End();
}

void draw_about_window() {
    if (!ImGui::Begin("About", &show_about, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    ImGui::Text("King's Field (SLPS-00017) source port - PC extensions");
    ImGui::Separator();
    ImGui::BulletText("Source port: sushi-shi/kings-field-decomp (CC0)");
    ImGui::BulletText("Overlay, settings and mod workflow inspired by Verdite2 (Voicedrew11, MIT)");
    ImGui::BulletText("English translation: John Osborne, Kaz; save fix: HwitVlf");
    ImGui::BulletText("Dear ImGui (MIT), Lua 5.4 (MIT), cJSON (MIT), SDL3 (zlib)");
    ImGui::End();
}

void draw_menu_bar() {
    if (!ImGui::BeginMainMenuBar())
        return;
    ImGui::MenuItem("Settings", nullptr, &show_settings);
    ImGui::MenuItem("Mods", nullptr, &show_mods);
    ImGui::MenuItem("Console", nullptr, &show_console);
    ImGui::MenuItem("About", nullptr, &show_about);
    // F1 on a keyboard; on a touch screen this is the way out.
    if (ImGui::MenuItem("Close"))
        set_open(false);
    char status[96];
    std::snprintf(status, sizeof status, "%d mod%s running   F1 to close%s", mods_loaded_count(),
                  mods_loaded_count() == 1 ? "" : "s", settings().pause_when_open ? "   (paused)" : "");
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(status).x - 16);
    ImGui::TextDisabled("%s", status);
    ImGui::EndMainMenuBar();
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
static bool covered() { return open || settings_panel_open(); }

bool overlay_open() { return covered(); }
bool overlay_captures_input() { return covered(); }
// Android's panel fills the screen, so the game is suspended under it, music and all.
bool pause_requested() {
    return settings_panel_open() || (open && settings().pause_when_open)
        || (automap_full_open() && settings().map_pause);
}
bool pause_keeps_music() {
    return !settings_panel_open() && !(open && settings().pause_when_open) && automap_full_open()
        && settings().map_keep_music;
}
// Nothing of the game shows under Android's panel, so it is not redrawn.
bool redraw_while_paused() { return open || automap_full_open(); }
bool wants_cursor() { return covered() || automap_full_open(); }

ViewRect game_view() {
    int width = 0, height = 0;
    if (main_window)
        SDL_GetWindowSizeInPixels(main_window, &width, &height);
    const auto rect = renderer_present_rect(width, height);
    return {static_cast<float>(rect.x), static_cast<float>(rect.y), static_cast<float>(rect.width),
            static_cast<float>(rect.height)};
}

ViewRect original_view() {
    const auto whole = game_view();
    const int extra = renderer_extra_columns();
    const float column = whole.w / static_cast<float>(render_width + 2 * extra);
    return {whole.x + column * static_cast<float>(extra), whole.y,
            column * static_cast<float>(render_width), whole.h};
}

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
        return true;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    static std::string ini_path = (user_dir() / "overlay.ini").string();
    io.IniFilename = ini_path.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    load_font();
    apply_style();
    if (!ImGui_ImplSDL3_InitForVulkan(handle) || !renderer_overlay_start()) {
        std::printf("[overlay] cannot initialize Dear ImGui\n");
        return false;
    }
    running = true;
    apply_display_settings();
    apply_audio_settings();
    apply_control_settings();
    apply_render_settings();
    std::printf("[overlay] ready - press F1 for settings, mods and console. User folder: %s\n",
                user_dir().string().c_str());
    mods_start();
    discord_start();
    fps_started = SDL_GetTicks();
    return true;
}

void shutdown() {
    if (!running)
        return;
    mods_shutdown();
    discord_stop();
    settings_save();
    running = false;
    if (!imgui)
        return;
    renderer_overlay_stop();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

bool process_event(const SDL_Event &event) {
    // Android may end the process in the background without an exit; the game sees the event too.
    if (event.type == SDL_EVENT_WILL_ENTER_BACKGROUND && running)
        settings_save();
    if (!running || !imgui)
        return false;
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
        const bool alt_enter = event.key.key == SDLK_RETURN && (event.key.mod & SDL_KMOD_ALT);
        if (event.key.key == SDLK_F11 || alt_enter) {
            settings().fullscreen = !settings().fullscreen;
            apply_display_settings();
            settings_mark_dirty();
            return true;
        }
        // M: full floor map (the game has no binding for it).
        if (event.key.key == SDLK_M && !open)
            automap_toggle_full();
        if (event.key.key == SDLK_F1 || (open && event.key.key == SDLK_ESCAPE && !ImGui::GetIO().WantTextInput)) {
            set_open(!open);
            return true;
        }
    }
    if (event.type == SDL_EVENT_KEY_UP && (event.key.key == SDLK_F1 || event.key.key == SDLK_F11))
        return true;
    if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
        apply_render_settings();
    ImGui_ImplSDL3_ProcessEvent(&event);
    // The full map takes the mouse (zoom, drag, hover); the keyboard still reaches the game.
    const bool mouse_event = event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
        || event.type == SDL_EVENT_MOUSE_BUTTON_UP || event.type == SDL_EVENT_MOUSE_WHEEL;
    if (automap_full_open() && mouse_event)
        return true;
    return covered() && is_input_event(event.type);
}

void poll() {
    if (!running)
        return;
    mods_poll();
    options_poll();
    settings_panel_poll();
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

static void draw_message(int width, int height) {
    constexpr Uint64 shown_ms = 1800, fade_ms = 400;
    const Uint64 age = SDL_GetTicks() - message_started;
    if (message_text.empty() || age >= shown_ms)
        return;
    const float alpha = age + fade_ms > shown_ms ? static_cast<float>(shown_ms - age) / fade_ms : 1.0f;
    ImFont *font = ImGui::GetFont();
    const float size = std::max(18.0f, height / 30.0f);
    const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, message_text.c_str());
    const ImVec2 at((width - extent.x) * 0.5f, height * 0.2f);
    ImDrawList *draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(ImVec2(at.x - size * 0.6f, at.y - size * 0.3f), ImVec2(at.x + extent.x + size * 0.6f, at.y + extent.y + size * 0.3f),
                        IM_COL32(0, 0, 0, static_cast<int>(150 * alpha)), size * 0.3f);
    draw->AddText(font, size, at, IM_COL32(255, 235, 170, static_cast<int>(255 * alpha)), message_text.c_str());
}

ImDrawData *build_overlay(int width, int height) {
    if (!running || !imgui || width <= 0 || height <= 0)
        return nullptr;
    ++fps_frames;
    const Uint64 now = SDL_GetTicks();
    if (now - fps_started >= 1000) {
        fps_value = static_cast<float>(fps_frames) * 1000.0f / static_cast<float>(now - fps_started);
        fps_frames = 0;
        fps_started = now;
    }
    // The Vulkan renderer's NewFrame only checks that it was started, so it is not called.
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    // KF_TEST_SETTINGS=<page>: opens Settings on that page, for captures; Android's panel on its first page.
    static const char *test_page = std::getenv("KF_TEST_SETTINGS");
    if (test_page) {
        if (!settings_panel_show())
            open = show_settings = true;
        show_mods = false;
        settings_page = test_page;
        test_page = nullptr;
        // KF_TEST_LANGUAGE_TASK=download: presses "Download English patch".
        if (const char *task = std::getenv("KF_TEST_LANGUAGE_TASK"); task && std::strcmp(task, "download") == 0)
            language_task_download_english();
    }
    // After mod HUDs, so the full map covers them.
    mods_draw_hud();
    automap_draw(last_context);
    item_description_draw();
    touch_controls_draw();
    draw_message(width, height);
    if (settings().show_fps) {
        char text[32];
        std::snprintf(text, sizeof text, "%.0f fps", fps_value);
        ImGui::GetForegroundDrawList()->AddText(ImVec2(8, open ? 30.0f : 6.0f), IM_COL32(255, 255, 120, 255), text);
    }
    if (open) {
        draw_menu_bar();
        if (show_settings)
            draw_settings_window();
        if (show_mods)
            draw_mods_window();
        if (show_console)
            draw_console_window();
        if (show_about)
            draw_about_window();
    }
    ImGui::Render();
    return ImGui::GetDrawData();
}
}
