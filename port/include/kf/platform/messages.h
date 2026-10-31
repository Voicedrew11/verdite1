#ifndef KF_PLATFORM_MESSAGES_H
#define KF_PLATFORM_MESSAGES_H
// PC extension: messages on the ground (docs/MESSAGES_PLAN.md). What the game asks while it plays:
// the marks to draw, whether the action button opens one, and the one being read. Kept by
// port/ext/messages_board.cpp; with online play off or offline, there are no marks and nothing
// opens, so the game plays as it did.
#include <kf/lib/types.h>
#include <kf/platform/language.h>

#include <optional>
#include <string>
#include <vector>

namespace kf {

struct GroundMark {
    u8 cell_x, cell_z;   // the cell the message lies in, as AutomapSnapshot's cells
    u8 facing;           // where its writer looked, in 256ths of a turn as yaw
    bool mine;           // this player's own: drawn the same, so a player can find what they left
};
// The marks on floor (1..5): this session's shown fraction of its messages.
std::vector<GroundMark> messages_marks(int floor);

// The action button, pressed by the player at (x, z) in cells looking along yaw (radians, as
// AutomapSnapshot): true when it was for a message, and the game must not act on it. A message is
// for it when it lies in the player's cell or the next one ahead. A press while one is open rates
// it good (once; never the player's own) and closes it.
bool messages_action(int floor, float x, float z, float yaw);

// A piece of the open message, as the pickup feed draws a line: text menu_spell spells, or an
// item's name (an id below KF_ITEM_COUNT) as the menus draw it.
inline constexpr u8 message_text_only = 0xff;
struct MessageTextPiece {
    std::string text;
    u8 item = message_text_only;
};
struct MessageReading {
    std::vector<MessageTextPiece> pieces;
    std::string prompt;   // the line under it, in language: what the action button does now
};
// The message open at (x, z) on floor, in language, or none. It closes on its own once the player
// is more than a cell from it.
std::optional<MessageReading> messages_reading(Language language, int floor, float x, float z);

} // namespace kf

#endif // KF_PLATFORM_MESSAGES_H
