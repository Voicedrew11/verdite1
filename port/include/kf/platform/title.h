#ifndef KF_PLATFORM_TITLE_H
#define KF_PLATFORM_TITLE_H
// PC extension: the title menu under the KING'S FIELD logo, as Verdite2 and Verdite3 have the
// game's own (verdite/title_menu.h has the menu). The opening's last scene asks for it once the
// logo is up, and holds the logo until the player has chosen how to start. Kept by
// port/ext/title.cpp, which reads the saves and coop worlds, keeps the room list open while it is
// shown and joins a room for a co-op choice.
#include <kf/lib/types.h>
#include <kf/platform/language.h>

#include <optional>
#include <string>
#include <vector>

namespace kf {

// How the game starts once the player has chosen. Continue's slot and Coop's room are the port's:
// the game starts as for a new game and the port puts the save, or the coop world, in place at
// its first point between updates (as F4's quick load does from the title).
enum class TitleStart : u8 { NewGame, Continue, Coop };

// Opens the menu: CONTINUE first when a card slot is used.
void title_open();

// Once per title frame, with the buttons newly pressed this frame; the keyboard's typing and the
// pointer are read by the port. The start, once chosen and, for co-op, once the room is joined.
std::optional<TitleStart> title_update(u32 pressed);

// What to draw this frame, spelled for menu_spell in the game's language. Positions and boxes are
// the game's; a row's text is its whole line.
struct TitleRowText {
    std::string text;
    std::string value;   // drawn at the row's right end; may be empty
    bool enabled = true;
    bool selected = false;
};
// A helmet and symbol as the player's profile has them: online_player_helmet_count's,
// online_player_symbol_count's and the colours' indexes.
struct TitleHelmet {
    u8 helmet = 0;
    u8 symbol = 0;
    u8 colour = 0;
};
struct TitleText {
    std::string heading;   // the page's title box; empty on the top page
    std::string code;      // a room's code or the player's name, new or typed so far, drawn under the heading
    std::vector<TitleRowText> rows;
    u32 grid_cells = 0;    // the join and name pages: their first rows are a grid of letters, this many,
    u32 grid_columns = 0;  // this many to a line
    std::vector<std::string> lines;   // words above the rows, centred, each its own line; an empty one is a gap
    std::string note;      // a line under the rows: the code typed, "CONNECTING", why a join failed
    std::optional<TitleHelmet> helmet;   // the PROFILE page: the helmet and symbol to show turning under the rows
};
TitleText title_text(Language language);

// True between title_open and the start: the overlay's F1 Online panel then leaves joining to the
// menu.
bool title_is_open();

// A key the host's keyboard typed while the title is open: a digit, a letter, or '\b' to erase.
// True when the join or name page took it, so it is not also a button.
bool title_type(char key);

// Between updates (host_between_updates): CONTINUE's slot goes in at the game's first.
void title_between_updates();

// Between updates: true once when a coop session started from the title has ended, and the game
// should go back to the title.
bool title_return_requested();

}

#endif // KF_PLATFORM_TITLE_H
