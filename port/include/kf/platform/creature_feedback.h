#ifndef KF_PLATFORM_CREATURE_FEEDBACK_H
#define KF_PLATFORM_CREATURE_FEEDBACK_H
// How a monster shows a blow and its death, as the later King's Fields do: a
// struck monster flushes the game's own damage red and eases back (III), and a
// slain one's body stays where it fell and fades away (II and III), where the
// original shows nothing on a hit and drops the body the moment its death ends.
//
// Watched, not told: each update the game hands over what every actor slot
// holds, so a blow from anywhere (the player, a spell, a coop partner's swing,
// a mod) flashes alike, and the game's own state is never changed. A body is
// drawn after the game has let go of its slot, from the slot as it was left.
#include <kf/lib/types.h>

#include <cstddef>

namespace kf {

// Settings: Gameplay > Enemies. Off in Classic mode.
inline bool creature_hit_flash = true;
inline bool creature_death_fade = true;

// An actor slot as one update finds it.
struct CreatureSighting {
    u8 definition = 0;      // what occupies the slot
    u16 health = 0;
    bool awake = false;     // the game updates and draws it (lifecycle active)
    bool dead = false;      // its death animation has played out (action post-death)
};

// How to draw a slot this update.
struct CreatureLook {
    bool drawn = false;
    s32 red_q12 = 0;        // how far its lighting goes toward the game's damage red, 4096 = all of it
    u8 keep = 255;          // how opaque it is drawn, 255 = as the game draws it
};

// Once per slot per drawn update, every slot, whether or not it is in view.
CreatureLook creature_look(std::size_t slot, const CreatureSighting &seen);
// A floor was entered or reloaded: no flash or body carries over.
void creature_feedback_forget();

} // namespace kf

#endif // KF_PLATFORM_CREATURE_FEEDBACK_H
