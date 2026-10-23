// F1 overlay: menu bar with Settings, Mods and Console windows, drawn with Dear
// ImGui over the game's frame. The game pauses while it is open (configurable).
#include "internal.hpp"
#include <backends/imgui_impl_opengl3.h>
#include <backends/imgui_impl_sdl3.h>
#include <imgui.h>
#include <imgui_internal.h>   // FindRenderedTextEnd
#include <kf/platform/host.h>
#include <kf/platform/language_runtime.h>
#include <kf/platform/translation_ppf.h>
#include <kf/renderer/renderer.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace kf::ext {
namespace {
SDL_Window *main_window;
bool running;
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
const char *settings_page = "Display";

void load_font() {
    ImGuiIO &io = ImGui::GetIO();
    const char *candidates[] = {"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf",
                                "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
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

// A file chosen in a dialog. The dialog answers later, possibly on another
// thread, so the page picks the result up on a later frame.
enum class Pick { None, Patch, Disc };
std::mutex pick_lock;
Pick pick_pending = Pick::None, pick_done = Pick::None;
std::string pick_path;

void pick_file(Pick purpose) {
    static const SDL_DialogFileFilter patch_filter[] = {{"English patch (.rar or .ppf)", "rar;ppf;zip;7z"}};
    static const SDL_DialogFileFilter disc_filter[] = {{"Disc images", "cue;bin;iso"}};
    {
        std::lock_guard guard(pick_lock);
        pick_pending = purpose;
    }
    SDL_ShowOpenFileDialog([](void *, const char *const *files, int) {
        std::lock_guard guard(pick_lock);
        pick_done = files && files[0] ? pick_pending : Pick::None;
        pick_path = files && files[0] ? files[0] : "";
        pick_pending = Pick::None;
    }, nullptr, main_window, purpose == Pick::Patch ? patch_filter : disc_filter, 1, nullptr, false);
}

// Settings > Language: the game's language, and setting up the other one.
void language_settings_page(bool &changed) {
    auto &s = settings();
    {
        std::lock_guard guard(pick_lock);
        if (pick_done == Pick::Patch)
            language_task_patch_file(pick_path);
        else if (pick_done == Pick::Disc)
            language_task_add_disc(pick_path);
        pick_done = Pick::None;
    }
    std::string message;
    const auto task = language_task_state(&message);
    const bool english = language_available(Language::English);
    const bool japanese = language_available(Language::Japanese);

    ImGui::TextWrapped("King's Field was released only in Japanese. The English version is John Osborne's fan "
                       "translation (v1.0). Saves work in both languages.");
    ImGui::Spacing();
    int choice = language_requested() == Language::English ? 1 : 0;
    ImGui::BeginDisabled(!english || !japanese);
    bool picked = ImGui::RadioButton("English", &choice, 1);
    ImGui::SameLine();
    picked |= ImGui::RadioButton("Japanese", &choice, 0);
    ImGui::EndDisabled();
    if (picked) {
        s.language = choice ? "en" : "ja";
        language_preference = choice ? Language::English : Language::Japanese;
        language_request(language_preference);
        changed = true;
    }
    if (language_requested() != game_language())
        ImGui::TextDisabled("Switches to %s when you return to the game.", language_name(language_requested()));
    else
        ImGui::TextDisabled("In use: %s. The game's Configuration menu can switch too.", language_name(game_language()));

    ImGui::SeparatorText("Game files");
    for (const auto language : {Language::English, Language::Japanese}) {
        const bool ready = language_available(language);
        ImGui::BulletText("%s: %s", language_name(language), ready ? "ready" : "not set up");
        if (ready)
            ImGui::SetItemTooltip("%s", language_folder(language).c_str());
    }
    ImGui::Spacing();
    const bool working = task == LanguageTask::Working;
    ImGui::BeginDisabled(working);
    if (!english && japanese) {
        ImGui::TextWrapped("Add English with the translator's patch, applied to your Japanese game files:");
        if (ImGui::Button("Download English patch"))
            language_task_download_english();
        ImGui::SetItemTooltip("Downloads John Osborne's release (about 6 MB) from the Internet Archive copy of\n"
                              "the translator's site, checks it, and builds English next to Japanese.");
        ImGui::SameLine();
        if (ImGui::Button("Choose patch file..."))
            pick_file(Pick::Patch);
        ImGui::SetItemTooltip("Kings_Field_Jap_to_Eng_v1.0.rar, or the .ppf inside it.");
    }
    if (!japanese) {
        ImGui::TextWrapped("Japanese needs your original Japanese disc image (.cue/.bin or .iso); a disc with the "
                           "English patch applied can't be turned back.");
        if (ImGui::Button("Add Japanese disc..."))
            pick_file(Pick::Disc);
    }
    ImGui::EndDisabled();
    if (working) {
        ImGui::TextDisabled("%s", message.c_str());
    } else if (task == LanguageTask::Done) {
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "%s", message.c_str());
        const auto other = game_language() == Language::English ? Language::Japanese : Language::English;
        if (language_available(other) && ImGui::Button(other == Language::English ? "Switch to English" : "Switch to Japanese")) {
            s.language = language_code(other);
            language_preference = other;
            language_request(other);
            language_task_clear();
            changed = true;
        }
    } else if (task == LanguageTask::Failed) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.45f, 1.0f), "%s", message.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::Button("OK"))
            language_task_clear();
    }
}

void draw_settings_window() {
    ImGui::SetNextWindowSize(ImVec2(560, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Settings", &show_settings)) {
        ImGui::End();
        return;
    }
    Settings &s = settings();
    bool classic_changed = false;
    if (ImGui::Checkbox("Classic mode", &s.classic)) {
        apply_render_settings();
        apply_control_settings();
        classic_changed = true;
    }
    ImGui::SetItemTooltip("Play King's Field as it originally shipped: 4:3 at 320x240, 20 fps,\n"
                          "polygon wiggle, texture warping, 15-bit colour with dithering, no added\n"
                          "lighting or anti-aliasing, the original look range, no run key, no\n"
                          "automap, equipment stats, quick save or bug fixes.\n\n"
                          "Your own settings are kept and come back when Classic mode is off.\n"
                          "Fullscreen, volume, controls, language and mods are unaffected.");
    ImGui::SameLine();
    if (s.classic)
        ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.45f, 1.0f), "On: as originally shipped. Your settings below are kept for later.");
    else
        ImGui::TextDisabled("(as originally shipped)");
    ImGui::Separator();
    const char *pages[] = {"Display", "Audio", "Controls", "Gameplay", "Language", "Map", "Interface"};
    ImGui::BeginChild("pages", ImVec2(130 * applied_scale, 0), ImGuiChildFlags_Borders);
    for (const char *page : pages)
        if (ImGui::Selectable(page, std::strcmp(settings_page, page) == 0))
            settings_page = page;
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("page");
    bool changed = classic_changed;
    if (std::strcmp(settings_page, "Display") == 0) {
        if (ImGui::Checkbox("Fullscreen", &s.fullscreen)) {
            apply_display_settings();
            changed = true;
        }
        ImGui::SetItemTooltip("Also Alt+Enter or F11");
        if (ImGui::Checkbox("VSync", &s.vsync)) {
            apply_display_settings();
            changed = true;
        }
        changed |= ImGui::Checkbox("Show FPS", &s.show_fps);
        ImGui::BeginDisabled(s.classic);   // Classic mode overrides the rest
        const int rates[] = {20, 0, 60, 120, 144, 165, 240, -1};
        const char *rate_labels[] = {"20 fps (original)", "Match display refresh", "60 fps", "120 fps", "144 fps",
                                     "165 fps", "240 fps", "Unlimited"};
        int rate_index = 1;
        for (int i = 0; i < 8; ++i)
            if (s.frame_rate == rates[i])
                rate_index = i;
        if (ImGui::Combo("Frame rate", &rate_index, rate_labels, 8)) {
            s.frame_rate = rates[rate_index];
            apply_render_settings();
            changed = true;
        }
        ImGui::SetItemTooltip("The game still updates 20 times a second; extra frames are drawn\nfrom a camera between updates, so the view trails by one update (50 ms).\nEnemies and objects still move in 20 fps steps for now.");
        ImGui::Spacing();
        const char *aspect_values[] = {"4:3", "16:9", "16:10", "21:9", "window"};
        const char *aspect_labels[] = {"4:3 (original)", "16:9", "16:10", "21:9", "Match window"};
        int aspect_index = 0;
        for (int i = 0; i < 5; ++i)
            if (s.aspect == aspect_values[i])
                aspect_index = i;
        if (ImGui::Combo("Aspect ratio", &aspect_index, aspect_labels, 5)) {
            s.aspect = aspect_values[aspect_index];
            apply_render_settings();
            changed = true;
        }
        ImGui::SetItemTooltip("Wider aspects show more of the world to each side.\nMenus and the HUD stay in the centre.");
        const char *resolution_labels[] = {"Native (match window)", "1x (320x240)", "2x", "3x", "4x", "5x", "6x", "7x", "8x"};
        int resolution_index = std::clamp(s.resolution, 0, 8);
        if (ImGui::Combo("Resolution", &resolution_index, resolution_labels, 9)) {
            s.resolution = resolution_index;
            apply_render_settings();
            changed = true;
        }
        const Renderer *renderer = host_renderer();
        ImGui::TextDisabled("Rendering %dx%d", renderer->logical_width * renderer->scale, render_height * renderer->scale);
        ImGui::Spacing();
        ImGui::SeparatorText("PlayStation look");
        // One switch for the whole effect; the parts stay adjustable below.
        bool wiggle = !s.subpixel && !s.perspective;
        if (ImGui::Checkbox("Polygon wiggle (original PS1 look)", &wiggle)) {
            s.subpixel = s.perspective = !wiggle;
            apply_render_settings();
            changed = true;
        }
        ImGui::SetItemTooltip("On: corners snap to the 320x240 grid and textures warp, as on the PlayStation.\nOff: smooth, stable polygons.");
        if (ImGui::TreeNode("Wiggle details")) {
            if (ImGui::Checkbox("Sub-pixel vertices", &s.subpixel)) {
                apply_render_settings();
                changed = true;
            }
            ImGui::SetItemTooltip("Keeps corners between pixels instead of snapping them to the\n320x240 grid. Removes most of the jitter as the camera moves.");
            if (ImGui::Checkbox("Perspective-correct textures", &s.perspective)) {
                apply_render_settings();
                changed = true;
            }
            ImGui::SetItemTooltip("Stops textures bending and swimming across large polygons.\nOff reproduces the PlayStation's affine texture mapping.");
            ImGui::TreePop();
        }
        if (ImGui::Checkbox("Smooth colours", &s.true_color)) {
            apply_render_settings();
            changed = true;
        }
        ImGui::SetItemTooltip("Full colour precision: smooth gradients in shadows and fog.\nOff: the PlayStation's 15-bit colour and its dither pattern.");
        const char *filter_labels[] = {"Nearest (original)", "Bilinear (smooth)", "3-point (N64 style)"};
        if (ImGui::Combo("Texture filtering", &s.texture_filter, filter_labels, 3)) {
            apply_render_settings();
            changed = true;
        }
        ImGui::SetItemTooltip("Smooths blocky textures on walls, floors and models.\nThe HUD and menus stay pixel-sharp.");
        const char *aa_labels[] = {"Off", "FXAA (fast)", "4x SSAA (sharp, heavier)", "9x SSAA (best, heavy)"};
        if (ImGui::Combo("Anti-aliasing", &s.antialiasing, aa_labels, 4)) {
            apply_render_settings();
            changed = true;
        }
        ImGui::SetItemTooltip("Smooths jagged polygon edges. FXAA is nearly free and leaves the HUD sharp.\nSSAA renders 4 or 9 times the pixels and averages them: cleanest, but costly.");
        ImGui::SeparatorText("Lighting");
        if (ImGui::Checkbox("Dynamic lighting", &s.dynamic_lights)) {
            apply_render_settings();
            changed = true;
        }
        ImGui::SetItemTooltip("Torches cast flickering light on nearby walls and floors,\nand fire and lightning spells light up what they pass.");
        if (s.dynamic_lights && slider_input("Light strength", &s.light_strength, 0, 200, "%d%%")) {
            apply_render_settings();
            changed = true;
        }
        if (s.dynamic_lights && ImGui::Checkbox("Walls block light", &s.light_occlusion)) {
            apply_render_settings();
            changed = true;
        }
        if (s.dynamic_lights)
            ImGui::SetItemTooltip("Stops torchlight reaching through solid walls into the next room.\nDoors and diagonal walls do not block it yet.");
        if (s.dynamic_lights && ImGui::Checkbox("Flame glow", &s.flame_glow)) {
            apply_render_settings();
            changed = true;
        }
        if (s.dynamic_lights)
            ImGui::SetItemTooltip("A soft halo around torch flames and fire or lightning spells.\nHidden when something is in front of the flame.");
        if (s.dynamic_lights && s.flame_glow && slider_input("Glow strength", &s.glow_strength, 0, 200, "%d%%")) {
            apply_render_settings();
            changed = true;
        }
        if (s.dynamic_lights && ImGui::Checkbox("Shadows", &s.shadows)) {
            apply_render_settings();
            changed = true;
        }
        if (s.dynamic_lights)
            ImGui::SetItemTooltip("The four nearest lights cast shadows. Shadows only remove the added\ntorchlight, so nothing gets darker than in the original game.");
        if (s.dynamic_lights && s.shadows && slider_input("Shadow strength", &s.shadow_strength, 0, 100, "%d%%")) {
            apply_render_settings();
            changed = true;
        }
        if (ImGui::Checkbox("Ambient occlusion", &s.ambient_occlusion)) {
            apply_render_settings();
            changed = true;
        }
        ImGui::SetItemTooltip("Soft shadowing in corners, where walls meet floors and under ledges.");
        if (s.ambient_occlusion && slider_input("Occlusion strength", &s.ao_strength, 0, 200, "%d%%")) {
            apply_render_settings();
            changed = true;
        }
        ImGui::SeparatorText("Accuracy");
        if (ImGui::Checkbox("Depth buffer (experimental)", &s.depth_buffer)) {
            apply_render_settings();
            changed = true;
        }
        ImGui::SetItemTooltip("Stops parts of objects showing through walls at some angles.\nThe PlayStation sorted whole polygons instead; some effects may\nlook different with this on.");
        ImGui::EndDisabled();
    } else if (std::strcmp(settings_page, "Audio") == 0) {
        if (slider_input("Volume", &s.volume, 0, 100, "%d%%")) {
            apply_audio_settings();
            changed = true;
        }
    } else if (std::strcmp(settings_page, "Map") == 0) {
        ImGui::BeginDisabled(s.classic);
        automap_settings_page(changed);
        ImGui::EndDisabled();
    } else if (std::strcmp(settings_page, "Language") == 0) {
        language_settings_page(changed);
    } else if (std::strcmp(settings_page, "Gameplay") == 0) {
        ImGui::BeginDisabled(s.classic);
        if (ImGui::Checkbox("Use carried keys on locked doors and chests", &s.auto_keys)) {
            apply_control_settings();
            changed = true;
        }
        ImGui::SetItemTooltip("Interacting with a lock unlocks it (with the unlock sound) when you carry\n"
                              "its key, instead of having to choose the key from the item menu.");
        ImGui::SeparatorText("Bug fixes");
        ImGui::TextWrapped("Fixes for bugs in the original game. Turn them off to play exactly as on the PlayStation.");
        ImGui::Spacing();
        if (ImGui::Checkbox("Enemies fall after hitting a ceiling mid-jump", &s.fix_ceiling_fall)) {
            apply_control_settings();
            changed = true;
        }
        ImGui::SetItemTooltip("The original meant jumping enemies that bump a low ceiling to drop back down,\nbut the check never matched, so they could be left hovering.");
        if (ImGui::Checkbox("Stereo panning for flagged sounds", &s.fix_spatial_panning)) {
            apply_control_settings();
            changed = true;
        }
        ImGui::SetItemTooltip("Some floor 5 enemy sounds are marked for a panning volume adjustment\nthat the original never applied.");
        ImGui::EndDisabled();
    } else if (std::strcmp(settings_page, "Controls") == 0) {
        ImGui::BeginDisabled(s.classic);
        const char *run_modes[] = {"Hold to run", "Toggle run"};
        int run_mode = s.run_toggle ? 1 : 0;
        if (ImGui::Combo("Run key (Shift)", &run_mode, run_modes, 2)) {
            s.run_toggle = run_mode == 1;
            apply_control_settings();
            changed = true;
        }
        ImGui::SetItemTooltip("Hold: run while Shift is held.\nToggle: each press switches between running and walking.");
        if (slider_input("Run speed", &s.run_speed, 100, 250, "%d%%")) {
            apply_control_settings();
            changed = true;
        }
        ImGui::SetItemTooltip("How fast you move while holding Shift (or clicking the left stick),\nas a percentage of walking speed. 100%% turns running off.");
        if (slider_input("Look up/down limit", &s.look_limit, 17, 85, "%d deg")) {
            apply_control_settings();
            changed = true;
        }
        ImGui::SetItemTooltip("How far the camera can tilt up or down. The original game allows about 17 degrees.");
        ImGui::EndDisabled();
        ImGui::Spacing();
        ImGui::TextWrapped("Current bindings (rebinding is planned):");
        const char *rows[][2] = {
            {"Move / strafe", "W A S D"}, {"Run", "Shift (hold or toggle)"}, {"Turn", "Left / Right arrows"}, {"Look", "Mouse, Page Up / Page Down"},
            {"Attack", "Space, left mouse"}, {"Magic", "Q, right mouse"}, {"Interact", "E, Enter"},
            {"Menu", "Tab (opens and backs out)"}, {"Back", "Backspace, Tab"},
            {"Quick save", "F2"}, {"Full map", "M"}, {"Quick load", "F4 (also on the title screen)"}, {"Pause", "P, Escape"}, {"Overlay", "F1"},
            {"Fullscreen", "F11, Alt+Enter"}};
        if (ImGui::BeginTable("bindings", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
            for (const auto &row : rows) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row[0]);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row[1]);
            }
            ImGui::EndTable();
        }
    } else {
        changed |= ImGui::Checkbox("Pause the game while the overlay is open", &s.pause_when_open);
        changed |= ImGui::Checkbox("Item descriptions in the game's menus", &s.item_descriptions);
        ImGui::SetItemTooltip("Under the item preview: what the item does, such as healing or an equipment effect.");
        // Applied when the edit finishes, so the window does not resize under the cursor.
        bool committed = false;
        slider_input("Interface scale", &s.ui_scale, 0.75f, 2.5f, "%.2f", 0.05f, &committed);
        ImGui::TextDisabled("Applies when you release the slider or press Enter. Mods are not scaled.");
        if (committed && s.ui_scale != applied_scale) {
            ImGui::GetStyle() = ImGuiStyle();
            apply_style();
            changed = true;
        }
    }
    ImGui::EndChild();
    if (changed)
        settings_mark_dirty();
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

bool overlay_open() { return open; }
bool overlay_captures_input() { return open; }
bool pause_requested() {
    return (open && settings().pause_when_open) || (automap_full_open() && settings().map_pause);
}
bool pause_keeps_music() {
    return !(open && settings().pause_when_open) && automap_full_open() && settings().map_keep_music;
}
bool redraw_while_paused() { return open || automap_full_open(); }
bool wants_cursor() { return open || automap_full_open(); }

ViewRect game_view() {
    int width = 0, height = 0;
    if (main_window)
        SDL_GetWindowSizeInPixels(main_window, &width, &height);
    const auto rect = renderer_present_rect(host_renderer(), width, height);
    return {static_cast<float>(rect.x), static_cast<float>(rect.y), static_cast<float>(rect.width),
            static_cast<float>(rect.height)};
}

ViewRect original_view() {
    const auto whole = game_view();
    const Renderer *renderer = host_renderer();
    const float column = whole.w / static_cast<float>(renderer->logical_width);
    return {whole.x + column * static_cast<float>(renderer_extra_columns(renderer)), whole.y,
            column * static_cast<float>(render_width), whole.h};
}

bool start(SDL_Window *handle, SDL_GLContext context) {
    main_window = handle;
    settings_load();
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    static std::string ini_path = (user_dir() / "overlay.ini").string();
    io.IniFilename = ini_path.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    load_font();
    apply_style();
    if (!ImGui_ImplSDL3_InitForOpenGL(handle, context) || !ImGui_ImplOpenGL3_Init("#version 300 es")) {
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
    fps_started = SDL_GetTicks();
    return true;
}

void shutdown() {
    if (!running)
        return;
    mods_shutdown();
    settings_mark_dirty();
    settings_save_if_dirty();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    running = false;
}

bool process_event(const SDL_Event &event) {
    if (!running)
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
    return open && is_input_event(event.type);
}

void poll() {
    if (!running)
        return;
    mods_poll();
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

void draw(int width, int height) {
    if (!running || width <= 0 || height <= 0)
        return;
    ++fps_frames;
    const Uint64 now = SDL_GetTicks();
    if (now - fps_started >= 1000) {
        fps_value = static_cast<float>(fps_frames) * 1000.0f / static_cast<float>(now - fps_started);
        fps_frames = 0;
        fps_started = now;
    }
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    // KF_TEST_SETTINGS=<page>: opens Settings on that page, for captures.
    static const char *test_page = std::getenv("KF_TEST_SETTINGS");
    if (test_page) {
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
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}
}
