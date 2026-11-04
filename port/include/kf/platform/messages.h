#ifndef KF_PLATFORM_MESSAGES_H
#define KF_PLATFORM_MESSAGES_H
// PC extension: messages on the ground (docs/MESSAGES_PLAN.md). What the game asks while it plays:
// the marks to draw, whether the action button opens one, and the box of the one being read. Kept by
// port/ext/messages_board.cpp; with online play off or offline, there are no marks and nothing
// opens, so the game plays as it did.
#include <kf/lib/types.h>
#include <kf/platform/language.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace kf {

// How a mark is tinted: not yet opened this session, opened or rated, or the player's own.
enum class GroundMarkKind : u8 { Unread, Read, Mine };
struct GroundMark {
    u8 cell_x, cell_z;   // the cell the message lies in, as AutomapSnapshot's cells
    u8 facing;           // where its writer looked, in 256ths of a turn as yaw
    GroundMarkKind kind;
};
// The marks on floor (1..5): this session's shown fraction of its messages.
std::vector<GroundMark> messages_marks(int floor);

// What stands in front of the player, as the game sees it. Something to act on takes the action
// button from any message there, so a message can never block a door, a person or a chest.
enum class PlayerFront : u8 {
    Clear,         // a message on the player's cell or the next one ahead can be read
    Wall,          // only one on the player's own cell: the next one ahead is behind a wall
    WorldTarget,   // none: the button is the game's
};
// Once per player update with no message open: the player at (x, z) in cells looking along yaw
// (radians, as AutomapSnapshot). True when a press of the action button would open a message, and
// the game draws the prompt while it is.
bool messages_reach(int floor, float x, float z, float yaw, PlayerFront front);
// The action button pressed while messages_reach said true: opens the message box.
void messages_open_reached();

// While a message box is open the player stands still, the world runs on, and the buttons are its.
bool messages_box_open();
enum class MessageBoxKey : u8 { Previous, Following, Confirm, Back };
void messages_box_press(MessageBoxKey key);
// A hit, a fall, or anything else that takes the player's attention away.
void messages_box_close();

// A piece of the open message, as the pickup feed draws a line: text menu_spell spells, or an
// item's name (an id below KF_ITEM_COUNT) as the menus draw it.
inline constexpr u8 message_text_only = 0xff;
struct MessageTextPiece {
    std::string text;
    u8 item = message_text_only;
};
// What the game draws for the open message, all in language.
struct MessageReading {
    std::vector<MessageTextPiece> pieces;
    std::string status;                 // who found it good and which of the cell's; may be empty
    std::vector<std::string> choices;   // the choices, in a row
    std::size_t cursor = 0;             // into choices
};
// The message open at (x, z) on floor, in language, or none. It closes on its own once the player
// is more than a cell from it.
std::optional<MessageReading> messages_reading(Language language, int floor, float x, float z);
// The line that says the action button would open a message; empty when it would not.
std::string messages_prompt(Language language);

// A line saying that other players rated this player's messages good, as the feed draws one.
struct MessageNotice {
    std::string text;
    float opacity = 1.0f;   // 1 shown .. 0 gone
};
std::vector<MessageNotice> messages_notices(Language language);

} // namespace kf

#endif // KF_PLATFORM_MESSAGES_H
