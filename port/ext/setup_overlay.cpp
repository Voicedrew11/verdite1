#include "setup_overlay.h"

#include "internal.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace kf::ext {
namespace {
using verdite::OptionValue;
using verdite::SetupChoice;
using verdite::SetupGroup;
using verdite::SetupPageView;
using verdite::SetupStep;

constexpr float heading_scale = 1.5f;
constexpr float card_title_scale = 1.3f;
constexpr float base_width_px = 1100.0f;
constexpr float base_height_px = 680.0f;
constexpr float base_viewport_height_px = 1080.0f;
// Below this many text lines of width, cards stack and labels sit over their controls.
constexpr float narrow_below_em = 44.0f;
constexpr float label_column = 0.30f;

constexpr const char *gpu_cost_note = "Higher GPU cost.";
constexpr const char *keep_label = "Keep as is";
constexpr const char *keep_help = "Set by hand. Choose one of these to replace it, or keep it as it is.";

ImU32 ink(ImGuiCol colour) { return ImGui::GetColorU32(colour); }

// Faked bold: the same text a pixel to the right, the font having one weight.
void bold_text(ImVec2 at, float size_px, const std::string &text) {
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddText(ImGui::GetFont(), size_px, at, ink(ImGuiCol_Text), text.c_str());
    draw->AddText(ImGui::GetFont(), size_px, ImVec2(at.x + 1.0f, at.y), ink(ImGuiCol_Text), text.c_str());
}

void dim_wrapped(const std::string &text) {
    if (text.empty())
        return;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

void focus_here(bool &first) {
    if (std::exchange(first, false))
        ImGui::SetKeyboardFocusHere();
}

// The heading, with the step at the right of its line, over a separator.
void heading(const std::string &title, const std::string &right) {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float line = ImGui::GetTextLineHeight();
    bold_text(at, ImGui::GetFontSize() * heading_scale, title);
    if (!right.empty()) {
        const float x = at.x + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(right.c_str()).x;
        ImGui::GetWindowDrawList()->AddText(ImVec2(x, at.y + (heading_scale - 1.0f) * 0.5f * line),
                                            ink(ImGuiCol_TextDisabled), right.c_str());
    }
    ImGui::Dummy(ImVec2(0, line * heading_scale));
    ImGui::Separator();
    ImGui::Spacing();
}

std::string step_text(const SetupPageView &view) {
    return "Step " + std::to_string(view.page + 1) + " of " + std::to_string(view.page_count + 1);
}

// ---- The welcome's cards ----

float card_pad() { return ImGui::GetFontSize() * 0.7f; }
float card_gap() { return ImGui::GetFontSize() * 0.3f; }
float card_chevron() { return ImGui::GetFontSize() * 1.5f; }

float card_height(float width, const verdite::SetupPreset &preset) {
    const float text_width = width - card_pad() * 2 - card_chevron();
    float height = card_pad() * 2 + ImGui::GetTextLineHeight() * card_title_scale + card_gap() +
                   ImGui::CalcTextSize(preset.help.c_str(), nullptr, false, text_width).y;
    if (!preset.summary.empty())
        height += card_gap() + ImGui::CalcTextSize(preset.summary.c_str(), nullptr, false, text_width).y;
    return height;
}

// A panel with its title, its description from the top, its display at the foot and a chevron, as
// every card leads on to the pages. An invisible button, so the keyboard and a pad reach it.
bool card(const verdite::SetupPreset &preset, ImVec2 size) {
    const bool clicked = ImGui::InvisibleButton(preset.id.c_str(), size);
    const bool lit = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const float round = ImGui::GetStyle().FrameRounding;
    const float pad = card_pad();
    const float text_width = size.x - pad * 2 - card_chevron();
    draw->AddRectFilled(min, max, ink(lit ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), round);
    draw->AddRect(min, max, ink(lit ? ImGuiCol_NavCursor : ImGuiCol_Border), round, 0, lit ? 2.5f : 1.0f);

    const ImVec2 title_at(min.x + pad, min.y + pad);
    bold_text(title_at, ImGui::GetFontSize() * card_title_scale, preset.label);
    const float help_y = title_at.y + ImGui::GetTextLineHeight() * card_title_scale + card_gap();
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(title_at.x, help_y), ink(ImGuiCol_Text), preset.help.c_str(),
                  nullptr, text_width);
    if (!preset.summary.empty()) {
        const float height = ImGui::CalcTextSize(preset.summary.c_str(), nullptr, false, text_width).y;
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(title_at.x, max.y - pad - height),
                      ink(ImGuiCol_TextDisabled), preset.summary.c_str(), nullptr, text_width);
    }
    const float half = ImGui::GetFontSize() * 0.35f;
    const ImVec2 tip(max.x - pad, max.y - pad - half);
    draw->AddTriangleFilled(ImVec2(tip.x - half, tip.y - half), tip, ImVec2(tip.x - half, tip.y + half), ink(ImGuiCol_Text));
    return clicked;
}

void draw_welcome(const SetupPageView &view, std::vector<SetupControl> &controls, bool &focus_first) {
    heading(view.welcome->title, "");
    ImGui::TextWrapped("%s", view.welcome->text.c_str());
    ImGui::Spacing();
    dim_wrapped(view.welcome->prompt);
    ImGui::Spacing();

    std::vector<const verdite::SetupPreset *> with_tier, without_tier;
    for (const verdite::SetupPreset *preset : view.presets)
        (preset->tier.empty() ? without_tier : with_tier).push_back(preset);

    const ImVec2 room = ImGui::GetContentRegionAvail();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const bool narrow = room.x < ImGui::GetFontSize() * narrow_below_em || with_tier.empty();
    const auto n = static_cast<float>(with_tier.size());
    const float column = narrow ? room.x : (room.x - spacing * (n - 1)) / n;

    float below = 0;
    for (const verdite::SetupPreset *preset : without_tier)
        below += card_height(room.x, *preset) + ImGui::GetStyle().ItemSpacing.y;
    float tallest = 0;
    for (const verdite::SetupPreset *preset : with_tier)
        tallest = std::max(tallest, card_height(column, *preset));
    // The row takes the height the rest leaves, but never less than its tallest card: a short
    // window scrolls the page instead of cutting a card's text.
    const float row_height = narrow ? tallest : std::max(tallest, room.y - below);

    const auto press = [&](const verdite::SetupPreset &preset, ImVec2 size) {
        if (card(preset, size))
            controls.emplace_back([id = preset.id](verdite::Setup &setup) { setup.choose_preset(id); });
    };
    for (std::size_t i = 0; i < with_tier.size(); ++i) {
        if (i > 0 && !narrow)
            ImGui::SameLine();
        // The last of the row (Modern) takes the first focus, wherever it sits.
        if (i + 1 == with_tier.size())
            focus_here(focus_first);
        press(*with_tier[i], ImVec2(column, narrow ? card_height(column, *with_tier[i]) : row_height));
    }
    for (const verdite::SetupPreset *preset : without_tier)
        press(*preset, ImVec2(room.x, card_height(room.x, *preset)));
}

// ---- The rows of a page ----

// Left or right on the item just drawn, when it has the focus and is not held: -1, 1 or 0. It
// steps a list or a slider in place, as a console menu does.
int step_input() {
    if (!ImGui::IsItemFocused() || ImGui::IsItemActive())
        return 0;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) ||
        ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft))
        return -1;
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) ||
        ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight))
        return 1;
    return 0;
}

// A stepper's end. Drawn on a dummy and hit-tested by the mouse: as a button the keyboard and the
// pad could focus it, and left and right would move there instead of stepping the list.
bool step_arrow(bool pointing_left, bool enabled, float size_px) {
    ImGui::Dummy(ImVec2(size_px, size_px));
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    const bool hot = enabled && ImGui::IsMouseHoveringRect(min, max);
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const float round = ImGui::GetStyle().FrameRounding;
    draw->AddRectFilled(min, max, ink(hot ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), round);
    draw->AddRect(min, max, ink(ImGuiCol_Border), round);
    const ImVec2 centre((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    const float half = size_px * 0.2f;
    const float dir = pointing_left ? -half * 0.5f : half * 0.5f;
    draw->AddTriangleFilled(ImVec2(centre.x + dir, centre.y), ImVec2(centre.x - dir, centre.y - half),
                            ImVec2(centre.x - dir, centre.y + half), ink(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled));
    return hot && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

// The per-frame facts a row needs.
struct RowPlace {
    float label_x;       // where a control starts, relative to the window
    float reserve_px;    // kept free at the right of each stepper, for the GPU note
    bool narrow;
    bool *focus_first;
    bool *list_open;
};

struct StepperResult {
    std::optional<std::size_t> picked;
    bool focused = false;
};

// "<", a list showing names[at] that opens to all of them, ">". Stops at the ends rather than wraps.
StepperResult stepper(const std::vector<std::string> &names, std::size_t at, const RowPlace &place) {
    const float arrow = ImGui::GetFrameHeight();
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    int delta = 0;
    if (step_arrow(true, at > 0, arrow))
        delta = -1;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(arrow * 3, ImGui::GetContentRegionAvail().x - arrow - gap - place.reserve_px));
    focus_here(*place.focus_first);
    StepperResult result;
    const bool listed = ImGui::BeginCombo("##list", names[at].c_str());
    result.focused = ImGui::IsItemFocused();
    if (listed) {
        *place.list_open = true;
        for (std::size_t i = 0; i < names.size(); ++i) {
            const bool selected = i == at;
            if (ImGui::Selectable(names[i].c_str(), selected) && !selected)
                result.picked = i;
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    } else {
        delta = delta != 0 ? delta : step_input();
    }
    ImGui::SameLine();
    if (step_arrow(false, at + 1 < names.size(), arrow))
        delta = 1;
    if (!result.picked && delta != 0) {
        const auto last = static_cast<int>(names.size()) - 1;
        const int to = std::clamp(static_cast<int>(at) + delta, 0, last);
        if (to != static_cast<int>(at))
            result.picked = static_cast<std::size_t>(to);
    }
    return result;
}

// A row's label, leaving the cursor where its control starts.
void row_label(const std::string &label, const RowPlace &place) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label.c_str());
    if (!place.narrow)
        ImGui::SameLine(place.label_x);
}

// The focus marker: a bar in the left gutter beside a row.
void gutter_bar(float top_px, float bottom_px) {
    // Half the padding is as far out as a window's clip lets anything draw.
    const float x = ImGui::GetWindowPos().x + ImGui::GetStyle().WindowPadding.x * 0.5f;
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x, top_px), ImVec2(x + 3.0f, bottom_px), ink(ImGuiCol_Text));
}

// Text under a control, at its left edge, wrapped short of the note's room.
void under_control(const std::string &text, bool dim, const RowPlace &place) {
    if (text.empty())
        return;
    if (!place.narrow)
        ImGui::SetCursorPosX(place.label_x);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - place.reserve_px);
    if (dim)
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text.c_str());
    if (dim)
        ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
}

void draw_gpu_note(const RowPlace &place) {
    if (place.narrow) {
        under_control(gpu_cost_note, true, place);
        return;
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", gpu_cost_note);
}

void draw_group(const verdite::SetupGroupView &view, std::vector<SetupControl> &controls, const RowPlace &place) {
    const SetupGroup &group = *view.group;
    ImGui::PushID(group.id.c_str());
    const float top = ImGui::GetCursorScreenPos().y;
    row_label(group.label, place);

    // Answers in the list's order: "Keep as is" first when it is one, then the choices.
    std::vector<std::string> names;
    if (view.keep_offered)
        names.emplace_back(keep_label);
    for (const SetupChoice &each : group.choices)
        names.push_back(each.label);
    const std::size_t offset = view.keep_offered ? 1 : 0;
    const std::size_t at = view.chosen ? *view.chosen + offset : 0;

    const StepperResult stepped = stepper(names, at, place);
    if (stepped.picked) {
        if (view.keep_offered && *stepped.picked == 0)
            controls.emplace_back([id = group.id](verdite::Setup &setup) { setup.keep(id); });
        else
            controls.emplace_back([id = group.id, i = *stepped.picked - offset](verdite::Setup &setup) { setup.choose(id, i); });
    }
    const SetupChoice *chosen = view.chosen ? &group.choices[*view.chosen] : nullptr;
    if (chosen && chosen->higher_gpu_cost)
        draw_gpu_note(place);
    under_control(chosen ? chosen->help : keep_help, !chosen, place);
    if (stepped.focused)
        gutter_bar(top, ImGui::GetItemRectMax().y);
    ImGui::PopID();
}

// A Choice row as a slider whose positions are its choices: the choice's label stands in the
// slider, typed entry is off.
bool choice_slider(const verdite::OptionRowView &view, const RowPlace &place, OptionValue &out) {
    const verdite::OptionRow &row = *view.row;
    int at = 0;
    for (std::size_t i = 0; i < row.choices.size(); ++i)
        if (row.choices[i].value == view.value)
            at = static_cast<int>(i);
    std::string label = verdite::option_value_label(row, view.value);
    for (std::size_t p = label.find('%'); p != std::string::npos; p = label.find('%', p + 2))
        label.insert(p, "%");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - place.reserve_px);
    focus_here(*place.focus_first);
    int moved = at;
    ImGui::SliderInt("##slider", &moved, 0, static_cast<int>(row.choices.size()) - 1, label.c_str(),
                     ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput);
    moved = std::clamp(moved + step_input(), 0, static_cast<int>(row.choices.size()) - 1);
    if (moved == at)
        return false;
    out = row.choices[static_cast<std::size_t>(moved)].value;
    return true;
}

bool number_slider(const verdite::OptionRowView &view, const RowPlace &place, OptionValue &out) {
    const verdite::OptionRow &row = *view.row;
    const auto was = std::get<double>(view.value);
    auto value = static_cast<float>(was);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - place.reserve_px);
    focus_here(*place.focus_first);
    const bool dragged = ImGui::SliderFloat("##slider", &value, static_cast<float>(row.min), static_cast<float>(row.max),
                                            row.format.empty() ? "%.2f" : row.format.c_str(),
                                            ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput);
    const double step = row.step > 0 ? row.step : (row.max - row.min) / 20.0;
    double now = dragged ? std::round(value / step) * step : was;
    now = std::clamp(now + step_input() * step, row.min, row.max);
    if (now == was)
        return false;
    out = now;
    return true;
}

void draw_option(const verdite::SetupOptionView &view, std::vector<SetupControl> &controls, const RowPlace &place) {
    const verdite::OptionRow &row = *view.row.row;
    const auto set = [&controls, id = row.id](OptionValue value) {
        controls.emplace_back([id, value = std::move(value)](verdite::Setup &setup) { setup.set_option(id, value); });
    };
    ImGui::PushID(row.id.c_str());
    const float top = ImGui::GetCursorScreenPos().y;
    row_label(row.label, place);
    bool focused = false;
    OptionValue moved;
    switch (row.kind) {
    case verdite::OptionKind::Toggle: {
        bool on = std::get<bool>(view.row.value);
        focus_here(*place.focus_first);
        if (ImGui::Checkbox("##on", &on))
            set(on);
        focused = ImGui::IsItemFocused();
        break;
    }
    case verdite::OptionKind::Choice: {
        if (view.look == verdite::SetupOptionLook::Slider) {
            if (choice_slider(view.row, place, moved))
                set(std::move(moved));
            focused = ImGui::IsItemFocused();
            break;
        }
        std::vector<std::string> names;
        std::size_t at = 0;
        for (std::size_t i = 0; i < row.choices.size(); ++i) {
            names.push_back(row.choices[i].label);
            if (row.choices[i].value == view.row.value)
                at = i;
        }
        const StepperResult stepped = stepper(names, at, place);
        if (stepped.picked)
            set(row.choices[*stepped.picked].value);
        focused = stepped.focused;
        break;
    }
    case verdite::OptionKind::Number:
        if (number_slider(view.row, place, moved))
            set(std::move(moved));
        focused = ImGui::IsItemFocused();
        break;
    case verdite::OptionKind::Action:
    case verdite::OptionKind::Status:
        break;
    }
    under_control(row.help, true, place);
    if (focused)
        gutter_bar(top, ImGui::GetItemRectMax().y);
    ImGui::PopID();
}

// A button, marked when it is the choice as well as lit, so the state is not colour alone. Its
// label is drawn over it, faked bold, so the labels read as bolder than the text under them.
bool lit_button(const std::string &label, bool lit, bool marked, ImVec2 size) {
    const std::string text = lit && marked ? "> " + label : label;
    ImGui::PushID(label.c_str());
    if (lit) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    const bool clicked = ImGui::Button("##button", size);
    if (lit)
        ImGui::PopStyleColor(2);
    ImGui::PopID();
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    const ImVec2 text_size = ImGui::CalcTextSize(text.c_str());
    bold_text(ImVec2(min.x + (max.x - min.x - text_size.x) * 0.5f, min.y + (max.y - min.y - text_size.y) * 0.5f),
              ImGui::GetFontSize(), text);
    return clicked;
}

float button_width(const std::string &label) {
    return std::max(ImGui::GetFontSize() * 6.5f,
                    ImGui::CalcTextSize(("> " + label).c_str()).x + ImGui::GetStyle().FramePadding.x * 2);
}

// "Set everything to:", each tier a button setting every row of the page to it, the one every row
// is at lit, and a dim "Custom" when they are mixed.
void draw_tier_row(const SetupPageView &view, std::vector<SetupControl> &controls, const RowPlace &place, bool &focus_first) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Set everything to:");
    for (std::size_t t = 0; t < view.tiers.size(); ++t) {
        const verdite::SetupTier &tier = *view.tiers[t];
        const float width = button_width(tier.label);
        if (!place.narrow || ImGui::GetContentRegionAvail().x > width)
            ImGui::SameLine();
        focus_here(focus_first);
        if (lit_button(tier.label, view.page_tier == t, true, ImVec2(width, 0)))
            controls.emplace_back([id = tier.id](verdite::Setup &setup) { setup.choose_page_tier(id); });
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s", tier.help.c_str());
    }
    if (!view.page_tier) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Custom");
    }
}

bool page_has_gpu_note(const SetupPageView &view) {
    return std::ranges::any_of(view.items, [](const verdite::SetupItemView &item) {
        const auto *group = std::get_if<verdite::SetupGroupView>(&item);
        return group && std::ranges::any_of(group->group->choices, &SetupChoice::higher_gpu_cost);
    });
}

void draw_page(const SetupPageView &view, std::vector<SetupControl> &controls, bool &focus_first, bool &list_open) {
    const verdite::SetupPage &page = *view.spec_page;
    heading(page.title, step_text(view));
    // A one-paragraph intro is secondary, so dim; a page explaining something at length is meant to be read.
    if (page.text.find("\n\n") == std::string::npos)
        dim_wrapped(page.text);
    else
        ImGui::TextWrapped("%s", page.text.c_str());
    ImGui::Spacing();

    const float width = ImGui::GetContentRegionAvail().x;
    RowPlace place{};
    place.narrow = width < ImGui::GetFontSize() * narrow_below_em;
    place.label_x = ImGui::GetCursorPosX() + width * label_column;
    place.reserve_px = !place.narrow && page_has_gpu_note(view)
                           ? ImGui::CalcTextSize(gpu_cost_note).x + ImGui::GetStyle().ItemSpacing.x
                           : 0.0f;
    place.focus_first = &focus_first;
    place.list_open = &list_open;
    if (!view.tiers.empty())
        draw_tier_row(view, controls, place, focus_first);
    for (const verdite::SetupItemView &item : view.items) {
        ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.25f));
        if (const auto *group = std::get_if<verdite::SetupGroupView>(&item))
            draw_group(*group, controls, place);
        else
            draw_option(std::get<verdite::SetupOptionView>(item), controls, place);
    }
}

void draw_summary(const SetupPageView &view) {
    heading("Ready", step_text(view));
    dim_wrapped("These choices will be applied:");
    ImGui::Spacing();
    const float value_x = ImGui::GetContentRegionAvail().x * 0.45f;
    for (const verdite::SetupSummarySection &section : view.summary) {
        ImGui::SeparatorText(std::string(section.title).c_str());
        for (const verdite::SetupSummaryLine &line : section.lines) {
            ImGui::TextUnformatted(line.label.data(), line.label.data() + line.label.size());
            ImGui::SameLine(value_x);
            ImGui::TextWrapped("%s", line.value.c_str());
        }
    }
}

// The input hints for the device used last.
std::string input_hints(bool pad, SetupStep step) {
    std::string hints = pad ? "A: Select" : "Enter: Select";
    if (step != SetupStep::Welcome)
        hints += pad ? "    B: Back" : "    Esc: Back";
    if (step == SetupStep::Page)
        hints += pad ? "    Start: Next" : "    PgDn: Next";
    if (step == SetupStep::Summary)
        hints += pad ? "    Start: Finish" : "    PgDn: Finish";
    return hints;
}

// The hints at the left, dim, where they fit; the buttons at the right, the way on filled in.
void draw_footer(const SetupPageView &view, std::vector<SetupControl> &controls, bool pad, bool &focus_first) {
    const verdite::SetupStep step = view.step;
    const std::string left = step == SetupStep::Welcome ? view.welcome->skip_label : "Back";
    const std::string right = step == SetupStep::Page ? "Next" : step == SetupStep::Summary ? "Finish" : "";
    const float left_width = button_width(left);
    const float right_width = right.empty() ? 0 : button_width(right);
    const float gap = right.empty() ? 0 : ImGui::GetStyle().ItemSpacing.x;
    const float room = ImGui::GetContentRegionAvail().x;
    const float buttons = left_width + gap + right_width;

    const std::string hints = input_hints(pad, step);
    if (ImGui::CalcTextSize(hints.c_str()).x + buttons + ImGui::GetFontSize() < room) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", hints.c_str());
        ImGui::SameLine(room - buttons + ImGui::GetStyle().WindowPadding.x);
    } else {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + room - buttons);
    }
    if (ImGui::Button(left.c_str(), ImVec2(left_width, 0))) {
        if (step == SetupStep::Welcome)
            controls.emplace_back([](verdite::Setup &setup) { setup.skip(); });
        else
            controls.emplace_back([](verdite::Setup &setup) { setup.back(); });
    }
    if (right.empty())
        return;
    ImGui::SameLine();
    if (step == SetupStep::Summary)
        focus_here(focus_first);
    if (lit_button(right, true, false, ImVec2(right_width, 0))) {
        if (step == SetupStep::Page)
            controls.emplace_back([](verdite::Setup &setup) { setup.next(); });
        else
            controls.emplace_back([](verdite::Setup &setup) { setup.finish(); });
    }
}

bool pad_pressed() {
    for (int key = ImGuiKey_GamepadStart; key <= ImGuiKey_GamepadRStickDown; ++key)
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(key), false))
            return true;
    return false;
}

bool keyboard_or_mouse_used() {
    const ImGuiIO &io = ImGui::GetIO();
    if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0 || ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        return true;
    for (int key = ImGuiKey_Tab; key <= ImGuiKey_KeypadEqual; ++key)
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(key), false))
            return true;
    return false;
}

// The page-wide keys: Escape and B step back, never off the welcome, where only its button skips;
// Start and Page Down go on. Not while a list was open or a slider held, as their own press closes them.
void page_shortcuts(SetupStep step, std::vector<SetupControl> &controls) {
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
        return;
    if (step != SetupStep::Welcome &&
        (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))) {
        controls.emplace_back([](verdite::Setup &setup) { setup.back(); });
        return;
    }
    if (!ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false) && !ImGui::IsKeyPressed(ImGuiKey_PageDown, false))
        return;
    if (step == SetupStep::Page)
        controls.emplace_back([](verdite::Setup &setup) { setup.next(); });
    else if (step == SetupStep::Summary)
        controls.emplace_back([](verdite::Setup &setup) { setup.finish(); });
}

} // namespace

void SetupOverlayScreen::show(const verdite::SetupPageView &view) {
    // A control on the same page keeps the focus where it is; only a new page moves it.
    focus_first_ = focus_first_ || !view_ || view_->step != view.step || view_->page != view.page;
    view_ = view;
}

void SetupOverlayScreen::draw(std::vector<SetupControl> &controls) {
    if (!view_)
        return;
    const SetupPageView &view = *view_;
    if (pad_pressed())
        pad_in_use_ = true;
    else if (keyboard_or_mouse_used())
        pad_in_use_ = false;

    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    const ImGuiStyle &style = ImGui::GetStyle();
    // Readable at 1440 and above: at 1 up to 1080 lines, larger past it, and the player's interface scale over that.
    const float scale = std::max(1.0f, viewport->Size.y / base_viewport_height_px);
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(base_width_px * scale * settings().ui_scale, viewport->Size.x * 0.95f),
                                    std::min(base_height_px * scale * settings().ui_scale, viewport->Size.y * 0.95f)),
                             ImGuiCond_Always);
    if (focus_first_)
        ImGui::SetNextWindowFocus();
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("Setup", nullptr, flags)) {
        ImGui::End();
        return;
    }
    ImGui::PushFont(nullptr, ImGui::GetFontSize() / (style.FontScaleMain * style.FontScaleDpi) * scale);
    const float footer = ImGui::GetFrameHeightWithSpacing() + style.ItemSpacing.y * 2;
    if (ImGui::BeginChild("page", ImVec2(0, -footer), ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AlwaysUseWindowPadding)) {
        bool list_open = false;
        switch (view.step) {
        case SetupStep::Welcome:
            draw_welcome(view, controls, focus_first_);
            break;
        case SetupStep::Page:
            draw_page(view, controls, focus_first_, list_open);
            break;
        case SetupStep::Summary:
            draw_summary(view);
            break;
        }
        const bool busy_now = list_open || ImGui::IsAnyItemActive();
        if (!list_was_open_ && !busy_now)
            page_shortcuts(view.step, controls);
        list_was_open_ = busy_now;
    }
    ImGui::EndChild();
    ImGui::Separator();
    draw_footer(view, controls, pad_in_use_, focus_first_);
    ImGui::PopFont();
    ImGui::End();
}

} // namespace kf::ext
