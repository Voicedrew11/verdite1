#ifndef KF_PLATFORM_ONLINE_PLAYERS_H
#define KF_PLATFORM_ONLINE_PLAYERS_H
// PC extension: online play. The other players in the online room, kept by the host
// (port/ext/online.cpp) for the game to draw in the world, and the knight they are drawn as
// (docs/PLAYER_MODEL.md).
#include <kf/lib/geometry_types.h>
#include <kf/lib/types.h>
#include <kf/renderer/textures.h>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace kf {
inline constexpr u8 online_no_weapon = 0xff;         // KF_OBJECT_NONE
inline constexpr s16 online_not_attacking = -1;      // KF_WEAPON_ATTACK_INACTIVE

struct OnlinePlayer {
    u32 id;           // the same while they stay in the room
    float x, z;       // in cells, as AutomapSnapshot (0..100)
    float yaw;        // radians, as AutomapSnapshot
    u8 weapon;        // the equipped weapon (0..15), or online_no_weapon
    s16 attack_phase; // 0..4095 through a swing, or online_not_attacking
    s16 pitch;        // the camera's, in 1/4096 turns, positive looking down
    s32 foot_height;  // the feet's y, up is negative
    float stride_phase; // 0..1 through the walk; the right foot leaves the ground at 0, the left at 0.5
    float stride;       // 0 standing .. 1 a full stride
    bool eyes;          // this player, down, sees through theirs: their knight is not drawn here
    u8 colour;          // the player's profile (port/ext/coop_profile.h): an index of the colours
    u8 helmet;          // and of the helmets
    u8 symbol;          // and of the tabard's symbols, each below its count
    std::string name;   // and their name, in what menu_spell spells
};

// Whether a name bar shows above the other players (kf/platform/online_name_bar.h): this player's choice.
enum class OnlineNameBars : u8 { Off, InSight, Always };   // Always: through walls too
OnlineNameBars online_name_bars();
// The game drew a name bar, counted for the agent channel's tests.
void online_name_bar_drawn();

// Whether the world keeps running while this player is held in a menu or a scripted loop: in a
// room (docs/COOP_PLAN.md), or with KF_TEST_WORLD_RUNS_WHILE_HELD=1 for tests without one.
bool online_world_runs_while_held();

// The other players on this floor (1..5) who are up, as last heard from; empty when not in a room.
std::vector<OnlinePlayer> online_players_on_floor(int floor);

// This player, as the others in the room are sent it.
struct OnlinePlayerPose {
    int floor;        // 1..5
    float x, z;       // in cells, as AutomapSnapshot
    float yaw;        // radians, as AutomapSnapshot
    s32 foot_height;  // the feet's y, up is negative
    u8 weapon;        // the equipped weapon (0..15), or online_no_weapon
    s16 attack_phase; // 0..4095 through a swing, or online_not_attacking
    s16 pitch;        // the camera's, in 1/4096 turns, positive looking down
};

// Once per world update, held or not: sends this player's pose to the others in the room, numbered
// by the update, and plays theirs on by one update (docs/PLAYER_MODEL.md).
void online_update(const OnlinePlayerPose &own);

// A sound this player's knight makes, which the others on its floor hear from where they draw it.
enum class OnlineSound : u8 { Swing, Death, KeyUnlock, StonePassage, Harp, WarpShimmer };
inline constexpr std::size_t online_sound_count = 6;
// This player made `sound` as the game played it: it goes to the others with the next pose.
void online_sound_made(OnlineSound sound);

// The knight, one TMD object per part in this order, each part's vertices relative to its pivot.
// A part comes after its parent; the right hand gives way to a held weapon's gauntlet.
enum class OnlinePlayerPart : u8 {
    Pelvis, Torso, Head, UpperArmRight, ForearmRight, UpperArmLeft, ForearmLeft,
    ThighRight, ShinRight, ThighLeft, ShinLeft, HandRight, HandLeft
};
inline constexpr std::size_t online_player_part_count = 13;

struct OnlinePlayerPartJoint {
    s8 parent;     // a part index, or -1 for the pelvis
    SVECTOR pivot; // in the model's space (the feet at 0, up is -y, facing -z)
};

// The knight's TMD (4-byte aligned, as the TMD reader requires) and its joints, in part order.
std::span<const u8> online_player_model_tmd();
std::span<const OnlinePlayerPartJoint, online_player_part_count> online_player_model_joints();
// Where the right forearm meets the hand, in the model's space: a held weapon's wrist.
SVECTOR online_player_model_right_wrist();

// The helmets the head can wear, the first the knight's great helm. Each is a TMD object after the
// parts, its vertices relative to the head's pivot, drawn with the head.
inline constexpr std::size_t online_player_helmet_count = 2;
u16 online_player_model_helmet_object(u8 helmet);
// The symbols a tabard's chest can carry, front and back, the first none. Each is the whole chest:
// a TMD object after the helmets, its vertices relative to the torso's pivot, drawn with the torso.
inline constexpr std::size_t online_player_symbol_count = 2;
u16 online_player_model_tabard_object(u8 symbol);
// What to sort a tabard by, nearer than it is: the breastplate's large faces would draw over its
// small ones.
s16 online_player_model_tabard_depth_bias();
// Where the TMD's primitives drawn in the player's colour hold it: each the offset of an R, G, B.
std::span<const u32> online_player_model_colour_offsets();
// And those drawn in the ink of the symbol on the tabard (CoopColour::ink).
std::span<const u32> online_player_model_ink_offsets();
// The knight's TMD in one of the profile's colours (port/ext/coop_profile.h; the first past them
// is the first), made the first time it is asked for and kept, since the renderer reads it until
// the frame is drawn.
std::span<const u8> online_player_tmd_in(u8 colour);
// Where the TMD's textured primitives hold their texture page.
std::span<const u32> online_player_model_page_offsets();

// The title's PROFILE page shows the helmet, but the opening's logo overlaps the metal's texture
// page. So the title puts the metal the helmets use elsewhere in the texture store, as these moves
// say, and draws from a copy of the TMD that looks for it there, made like online_player_tmd_in's.
std::span<const TextureMove> online_player_title_texture_moves();
std::span<const u8> online_player_title_tmd_in(u8 colour);
}

#endif // KF_PLATFORM_ONLINE_PLAYERS_H
