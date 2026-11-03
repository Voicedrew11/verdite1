#ifndef VERDITE_TITLE_MENU_H
#define VERDITE_TITLE_MENU_H
// The title menu under a game's logo, and its co-op pages, stepped with a pad, a keyboard or a
// pointer. The top page is one line across: CONTINUE (first, and where the cursor starts, when
// there is a save), NEW GAME, CO-OP and MODS. MODS lists the game's mods to switch on and off.
// CO-OP leads to hosting a world (a new private or public one, or one this
// machine keeps), joining a room by its code, the service's list of public rooms, and the
// player's profile: the name, colour and helmet the others in a room see.
//
// Game-agnostic and holding no text of its own: the game draws each page with its own menus and
// font, naming rows by their kind; the text it is handed (a save's summary, a host's name) is the
// game's or the service's, drawn as given. A profile's colour and helmet are indexes into the
// game's own lists.
//
// The menu knows no network. A choice is handed to the game once, by take_choice(). A co-op
// choice then shows the Waiting page until the game says it worked (the menu is done) or failed
// (back to the page it came from, saying why); Back on Waiting is a choice too: cancel the join.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace verdite {

// A private room's code is 8 digits, which every language's menu font can draw and a pad types on
// a short grid. A public room's is P and 7 of the letters and digits that cannot be taken for
// another (no 0, O, 1 or I); players never type or read it, the list joins it.
inline constexpr std::string_view private_code_digits = "0123456789";
inline constexpr std::string_view public_code_letters = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
inline constexpr std::size_t private_code_length = 8;
inline constexpr std::size_t public_code_length = 8;

std::string room_code_private(std::uint64_t random_bits);
std::string room_code_public(std::uint64_t random_bits);
// Either form.
bool room_code_valid(std::string_view code);
bool room_code_is_public(std::string_view code);

// A player's name: 1 to name_most capitals and digits, which the menu fonts draw.
inline constexpr std::string_view name_letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
inline constexpr std::size_t name_most = 12;
bool name_valid(std::string_view name);

// The player's profile, as PROFILE shows and changes it.
struct TitleProfile {
    std::string name;            // name_valid
    std::size_t colour = 0;      // < colours
    std::size_t colours = 1;
    std::size_t helmet = 0;      // < helmets
    std::size_t helmets = 1;
};

// A card slot as CONTINUE lists it.
struct TitleSave {
    bool used = false;
    std::string summary;   // the game's line for it ("LEVEL 12  FLOOR 3"); empty when unused
};

// A coop world this machine keeps (it hosted it before), offered again under Host.
struct TitleWorld {
    std::string code;
    std::uint32_t highest_floor = 0;
    std::uint32_t characters = 0;   // everyone who has played in it
};

// A public room as the service lists it.
struct TitleRoom {
    std::string code;
    std::string host;   // the host's name, checked by the service
    std::uint32_t floor = 0;
    std::uint32_t players = 0;
    std::uint32_t most = 0;   // players == most: drawn dimmed, not joined
};

enum class RoomListState : std::uint8_t { Connecting, Listing, Failed };

// A mod as MODS lists it. Failed and Held mods are switched on: Confirm switches them off.
enum class TitleModState : std::uint8_t { Off, On, Failed, Held };

struct TitleMod {
    std::string id;     // the game's, handed back by the choice
    std::string name;
    TitleModState state = TitleModState::Off;
};

enum class TitlePage : std::uint8_t {
    Top,        // CONTINUE, NEW GAME, CO-OP, MODS: a line across
    Continue,   // the card slots
    Coop,       // HOST, JOIN BY CODE, ROOM LIST, PROFILE
    Host,       // NEW PRIVATE WORLD, NEW PUBLIC WORLD, then the kept worlds
    HostCode,   // a new private world's code, to read out before START
    JoinCode,   // the code typed so far, and a grid of the digits with DELETE and JOIN
    RoomList,   // the public rooms
    Waiting,    // joining; Back cancels
    Profile,    // NAME, COLOUR, HELMET
    NameEntry,  // the name typed so far, and a grid of name_letters with DELETE and DONE
    Mods,       // the game's mods
};

enum class TitleRowKind : std::uint8_t {
    Continue, NewGame, Coop, Mods,                 // Top
    Slot,                                          // Continue: save
    Host, JoinCode, RoomList, Profile,             // Coop
    NewPrivateWorld, NewPublicWorld, KeptWorld,    // Host: world
    Start,                                         // HostCode
    Letter, Delete, Join,                          // JoinCode: letter
    Room,                                          // RoomList: room
    ProfileName, ProfileColour, ProfileHelmet,     // Profile: Left and Right step colour and helmet
    Done,                                          // NameEntry, after its letters and DELETE
    Mod,                                           // Mods: mod
};

struct TitleRowView {
    TitleRowKind kind = TitleRowKind::NewGame;
    bool enabled = true;   // false: drawn dimmed, Confirm refused (an empty slot, a full room)
    bool selected = false;
    char letter = 0;                     // Letter: a digit, or on NameEntry a name letter
    const TitleSave *save = nullptr;     // Slot
    const TitleWorld *world = nullptr;   // KeptWorld
    const TitleRoom *room = nullptr;     // Room
    const TitleMod *mod = nullptr;       // Mod
};

struct TitleView {
    TitlePage page = TitlePage::Top;
    std::vector<TitleRowView> rows;   // JoinCode, NameEntry: the letters in reading order, then the rest
    std::size_t grid_columns = 0;     // JoinCode, NameEntry: the letters' columns; 0 elsewhere
    std::string code;                 // HostCode, JoinCode and Waiting: the room's code; NameEntry: the name so far
    RoomListState list_state = RoomListState::Connecting;   // RoomList
    std::string failure;              // why the last co-op choice failed, until the next press
    TitleProfile profile;             // the player's, as last chosen
};

enum class TitleInput : std::uint8_t { Up, Down, Left, Right, Confirm, Back };

enum class TitleAction : std::uint8_t {
    NewGame,
    Continue,     // slot
    Host,         // code, listed
    Join,         // code
    CancelJoin,   // Back on Waiting: leave the room
    Profile,      // profile: the player changed it; the game keeps it
    EnableMod,    // code: the mod's id. The game answers with set_mods
    DisableMod,   // code: the mod's id
};

struct TitleChoice {
    TitleAction action = TitleAction::NewGame;
    std::size_t slot = 0;
    std::string code;      // Host, Join, CancelJoin: the room; EnableMod, DisableMod: the mod's id
    bool listed = false;   // Host: a public room
    TitleProfile profile;
};

class TitleMenu {
public:
    // random_seed draws new rooms' codes.
    explicit TitleMenu(std::uint64_t random_seed);

    // Starts at the top page, on CONTINUE when a save is used, else on NEW GAME.
    void open(std::vector<TitleSave> saves, std::vector<TitleWorld> worlds, TitleProfile profile,
              std::vector<TitleMod> mods);

    // On the top page Left and Right pick a row (wrapping) and Up and Down do nothing. Elsewhere Up
    // and Down pick a row (wrapping), Left and Right a column on the grid or a step of the colour
    // or helmet (wrapping, chosen at once). Confirm takes the row, Back goes up a page (and
    // on Top does nothing; on NameEntry it leaves the name as it was). DONE chooses the name.
    void press(TitleInput input);
    // A keyboard's digit on the join page, or letter or digit on the name page (lower case taken
    // as capitals); anything else is ignored, and the text stops at its length.
    void type(char letter);
    void erase();   // Backspace on the join or name page
    // The cursor on a row of this page, as a pointer chose it.
    void point(std::size_t row);

    // The service's latest list, while wants_room_list().
    void set_rooms(std::vector<TitleRoom> rooms, RoomListState state);
    bool wants_room_list() const;   // on RoomList: the game keeps the list open

    // The mods as they are now, after a choice or when they change while MODS shows them.
    void set_mods(std::vector<TitleMod> mods);

    // What the player chose, once.
    std::optional<TitleChoice> take_choice();
    // The co-op choice being waited on worked: the menu is done.
    void coop_started();
    // It failed: back to the page it was made on, saying why.
    void coop_failed(std::string why);

    // Its rows point into the menu: valid until the menu next changes.
    TitleView view() const;

private:
    void go(TitlePage page, std::optional<TitleRowKind> on);
    void move_vertically(int delta, std::size_t count);
    void move_sideways(int delta, std::size_t count);
    void move_in_grid(int delta);
    void step_profile(int delta);
    void choose_profile();
    void confirm();
    void wait_for(TitleChoice choice);
    void back();
    void return_from_waiting();

    std::uint64_t random_;
    TitlePage page_ = TitlePage::Top;
    TitlePage waiting_from_ = TitlePage::Top;
    std::size_t row_ = 0;
    std::size_t waiting_row_ = 0;
    std::vector<TitleSave> saves_;
    std::vector<TitleWorld> worlds_;
    std::vector<TitleRoom> rooms_;
    std::vector<TitleMod> mods_;
    RoomListState list_state_ = RoomListState::Connecting;
    std::string code_;
    std::string failure_;
    TitleProfile profile_;
    std::optional<TitleChoice> choice_;
};

} // namespace verdite

#endif // VERDITE_TITLE_MENU_H
