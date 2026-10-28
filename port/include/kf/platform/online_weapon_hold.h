#ifndef KF_PLATFORM_ONLINE_WEAPON_HOLD_H
#define KF_PLATFORM_ONLINE_WEAPON_HOLD_H

// How another player's knight holds its weapon (docs/PLAYER_MODEL.md, "The weapon"). The game's
// first-person model (WEPON/WEPxx.MIM) is three closed pieces but one: a forearm from the elbow
// (its origin), open at both ends and made for one view, a gauntlet, and the weapon. The knight
// draws the gauntlet and weapon, one rigid piece through the clip, at the size of the hit on the
// end of its own forearm, turned as the clip turns them; the arm follows where the clip has the
// wrist, and at rest holds the blade up in front.

#include <kf/lib/geometry_types.h>
#include <kf/lib/types.h>

#include <array>
#include <optional>
#include <span>
#include <vector>

namespace kf {

struct OnlineVector {
    float x, y, z;
};

// A rotation, its rows, in the knight's body: actor space, +y down and -z ahead.
using OnlineRotation = std::array<OnlineVector, 3>;

// A face of the model: its corners' vertices, 3 or 4 of them.
struct OnlineModelFace {
    std::array<u16, 4> vertices;
    u8 corners;
};

struct OnlineWeaponHold {
    OnlineRotation view;                 // the model's space into the body's
    std::array<u16, 3> followed;         // three vertices of the gauntlet and weapon that give their turn
    OnlineRotation hit_frame;            // what the followed vertices make at the hit, in the body
    OnlineVector wrist_from_followed;    // from the first followed vertex to the wrist at the hit, in the body
    SVECTOR wrist;                       // where the model's forearm meets the gauntlet, at the hit
    OnlineRotation rest;                 // the gauntlet and weapon at rest: blade up and ahead
    std::vector<u16> forearm;            // the model's forearm, not drawn
};

// The hold of a model, from its vertices at the hit and its faces: nothing when its pieces are not
// a forearm reaching the origin and others beside it, or a face names a vertex past the model's.
std::optional<OnlineWeaponHold> online_weapon_hold(std::span<const SVECTOR> hit_vertices,
    std::span<const OnlineModelFace> faces, const OnlineRotation &view);

// The weapon arm, each part's rotation in the body.
struct OnlineWeaponArm {
    OnlineRotation upper_arm;
    OnlineRotation forearm;   // its rest direction is +y, down from the elbow
    OnlineRotation held;      // the gauntlet and weapon, from how the model is at the hit
};

// The arm at rest, its upper arm and forearm as the stance has them.
OnlineWeaponArm online_weapon_arm_at_rest(const OnlineWeaponHold &hold, const OnlineRotation &upper_arm,
    const OnlineRotation &forearm);

// The arm through a swing, from rest by attack_weight, with the followed vertices where the clip
// has them at that phase: the gauntlet and weapon turned as the clip turns them, the forearm from
// the elbow toward the clip's wrist, the upper arm lifted to most of the forearm's angle from
// hanging down, past overhead when it points up and back.
OnlineWeaponArm online_weapon_arm_swinging(const OnlineWeaponHold &hold, const OnlineWeaponArm &rest,
    std::span<const SVECTOR, 3> followed_vertices, float attack_weight);

// How far an attack has turned the arm from rest into the swing: in over its first phases, out over
// its last, so it starts and ends at rest.
float online_attack_weight(s16 attack_phase);

// Two rotations composed: inner, then outer. And to and from the game's fixed-point matrices.
OnlineRotation online_compose(const OnlineRotation &outer, const OnlineRotation &inner);
OnlineRotation online_rotation(const MATRIX &matrix);
MATRIX online_matrix(const OnlineRotation &rotation);

}

#endif // KF_PLATFORM_ONLINE_WEAPON_HOLD_H
