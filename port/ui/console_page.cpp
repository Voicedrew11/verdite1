// The F1 menu's Console page: what the game and its mods printed (console.cpp), filtered, with Clear
// and Copy.
#include "menu_page.h"

#include "internal.hpp"

#include <verdite/rml_text.h>
#include <verdite/ui.h>

#include <SDL3/SDL.h>

#include <cstdio>
#include <ranges>

namespace kf::ext {
namespace {
constexpr std::size_t kept_lines = 8000;    // copied from the console
constexpr std::size_t shown_lines = 400;    // the newest that match, written into the page

enum class ConsoleRow : std::uint8_t { Clear, Copy, Follow, Filter };
constexpr ConsoleRow console_rows[] = {ConsoleRow::Clear, ConsoleRow::Copy, ConsoleRow::Follow, ConsoleRow::Filter};

const char *line_class(const std::string &text) {
    if (text.find("error") != std::string::npos || text.find("Error") != std::string::npos)
        return "error";
    if (text.rfind("[mods]", 0) == 0)
        return "mods";
    return "";
}

class ConsolePage final : public MenuPage {
public:
    std::string title() const override { return "Console"; }

    int row_count() const override { return static_cast<int>(std::size(console_rows)); }
    verdite::MenuRowShape shape(int row) const override {
        if (row == static_cast<int>(ConsoleRow::Follow))
            return {verdite::MenuRowKind::Toggle, true, 2, follow_ ? 1 : 0};
        return {verdite::MenuRowKind::Action};
    }
    void choose(int row, int choice) override {
        if (row == static_cast<int>(ConsoleRow::Follow))
            follow_ = choice == 1;
    }
    void activate(int row, verdite::ui::Document &page) override {
        switch (static_cast<ConsoleRow>(row)) {
        case ConsoleRow::Clear:
            lines_.clear();
            console_clear();
            break;
        case ConsoleRow::Copy: copy(); break;
        case ConsoleRow::Follow: follow_ = !follow_; break;
        case ConsoleRow::Filter: page.focus_field("console-filter"); break;
        }
    }
    std::string rows_rml(const verdite::MenuModel &model) const override {
        std::string out = menu_row_rml(model, 0, "Clear", "", "action");
        out += menu_row_rml(model, 1, "Copy", "", "action");
        out += menu_row_rml(model, 2, "Follow new lines", follow_ ? "<value>On</value>" : "<value>Off</value>",
                            follow_ ? "toggle on" : "toggle");
        out += menu_row_rml(model, 3, "Filter", "", "action");
        return out;
    }

    std::string part_rml() const override {
        return "<div id=\"console-filter-box\"><input type=\"text\" id=\"console-filter\" maxlength=\"120\"/></div>"
               "<div id=\"console-lines\"></div>"
               "<p id=\"console-log\" class=\"dim\"></p>";
    }
    void update(verdite::ui::Document &page) override {
        serial_ = console_copy(serial_, lines_);
        if (lines_.size() > kept_lines)
            lines_.erase(lines_.begin(), lines_.end() - static_cast<std::ptrdiff_t>(kept_lines));
        const std::string filter = page.field("console-filter");
        std::vector<const ConsoleLine *> matching;
        for (auto line = lines_.rbegin(); line != lines_.rend() && matching.size() < shown_lines; ++line)
            if (filter.empty() || line->text.find(filter) != std::string::npos)
                matching.push_back(&*line);
        std::string rml;
        for (const ConsoleLine *line : std::views::reverse(matching))
            rml += std::string("<p class=\"line ") + line_class(line->text) + "\">" +
                   verdite::rml_escaped(line->text) + "</p>";
        const bool changed = rml != shown_;
        shown_ = std::move(rml);
        page.set_rml("console-lines", shown_);
        if (changed && follow_)
            page.scroll_to_end("console-lines");
        page.set_text("console-log", "log: " + (user_dir() / "kf.log").string());
    }

private:
    void copy() const {
        std::string all;
        for (const ConsoleLine &line : lines_)
            all += line.text + "\n";
        if (!SDL_SetClipboardText(all.c_str()))
            std::fprintf(stderr, "[ui] the clipboard: %s\n", SDL_GetError());
    }

    std::vector<ConsoleLine> lines_;
    std::uint64_t serial_ = 0;
    std::string shown_;
    bool follow_ = true;
};
} // namespace

std::unique_ptr<MenuPage> make_console_page() {
    return std::make_unique<ConsolePage>();
}

} // namespace kf::ext
