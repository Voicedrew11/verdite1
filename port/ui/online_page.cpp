// The F1 menu's Online page: the room, who is here, or a room code to join (online.cpp's OnlinePanel).
#include "menu_page.h"

#include "internal.hpp"

#include <verdite/rml_text.h>
#include <verdite/ui.h>

namespace kf::ext {
namespace {
constexpr const char *intro =
    "Play together: everyone who joins the same room code, with the same build of the game, plays one coop "
    "world, kept by whoever is in the room first. Joining sets this game aside; leaving puts it back.";

// What its rows are now.
enum class OnlineRow : std::uint8_t { Code, Join, Leave };

class OnlinePage final : public MenuPage {
public:
    std::string title() const override { return "Online"; }

    int row_count() const override { return static_cast<int>(rows().size()); }
    verdite::MenuRowShape shape(int) const override { return {verdite::MenuRowKind::Action}; }
    void activate(int row, verdite::ui::Document &page) override {
        const std::vector<OnlineRow> now = rows();
        if (row < 0 || static_cast<std::size_t>(row) >= now.size())
            return;
        switch (now[static_cast<std::size_t>(row)]) {
        case OnlineRow::Code: page.focus_field("room-code"); break;
        case OnlineRow::Join: join(page); break;
        case OnlineRow::Leave: online_panel_leave(); break;
        }
    }
    std::string rows_rml(const verdite::MenuModel &model) const override {
        std::string out;
        const std::vector<OnlineRow> now = rows();
        for (std::size_t i = 0; i < now.size(); ++i) {
            const int index = static_cast<int>(i);
            switch (now[i]) {
            case OnlineRow::Code: out += menu_row_rml(model, index, "Type a room code", "", "action"); break;
            case OnlineRow::Join: out += menu_row_rml(model, index, "Join", "", "action"); break;
            case OnlineRow::Leave: out += menu_row_rml(model, index, "Leave", "", "action"); break;
            }
        }
        return out;
    }

    std::string part_rml() const override {
        return std::string("<div class=\"page-text\">") + intro + "</div>"
               "<p id=\"online-note\" class=\"dim\"></p>"
               "<div id=\"online-room\"></div>"
               "<div id=\"online-join\"><input type=\"text\" id=\"room-code\" maxlength=\"64\" "
               "data-enter=\"join\"/></div>"
               "<p id=\"online-status\" class=\"dim\"></p>"
               "<p id=\"online-service\" class=\"dim\"></p>";
    }
    void update(verdite::ui::Document &page) override {
        panel_ = online_panel();
        if (!panel_.built) {
            page.set_text("online-note", "This build has no online play: it was built without libdatachannel.");
            page.set_classes("online-join", "hidden");
            return;
        }
        std::string note = panel_.mods_stay_off ? "Your mods stay off while you are in a room." : "";
        if (!panel_.in_room && panel_.at_title)
            note += (note.empty() ? "" : " ") + std::string("On the title screen, choose CO-OP to host or join a room.");
        page.set_text("online-note", note);
        page.set_rml("online-room", room_rml());
        page.set_classes("online-join", can_join() ? "" : "hidden");
        page.set_text("online-status", panel_.in_room ? "" : panel_.status);
        page.set_text("online-service", "Service: " + panel_.service);
    }
    void action(const verdite::ui::Action &action, verdite::ui::Document &page) override {
        if (action.kind == verdite::ui::ActionKind::Enter && action.name == "join")
            join(page);
    }

private:
    bool can_join() const { return panel_.built && !panel_.in_room && !panel_.at_title; }

    std::vector<OnlineRow> rows() const {
        if (panel_.in_room)
            return {OnlineRow::Leave};
        if (can_join())
            return {OnlineRow::Code, OnlineRow::Join};
        return {};
    }

    void join(verdite::ui::Document &page) {
        const std::string code = page.field("room-code");
        page.blur();
        online_panel_join(code);
    }

    std::string room_rml() const {
        if (!panel_.in_room)
            return {};
        std::string out = "<p>Room: " + verdite::rml_escaped(panel_.room) + "</p>";
        if (!panel_.coop_line.empty())
            out += "<p>" + verdite::rml_escaped(panel_.coop_line) + "</p>";
        if (panel_.joining)
            out += "<p class=\"dim\">Joining...</p>";
        else if (panel_.players.empty())
            out += "<p class=\"dim\">Waiting for someone to join the same room.</p>";
        for (const std::string &player : panel_.players)
            out += "<p class=\"player\">" + verdite::rml_escaped(player) + "</p>";
        return out;
    }

    OnlinePanel panel_;
};
} // namespace

std::unique_ptr<MenuPage> make_online_page() {
    return std::make_unique<OnlinePage>();
}

} // namespace kf::ext
