#ifndef KF_PLATFORM_CONTENT_H
#define KF_PLATFORM_CONTENT_H
// PC extension: what game code asks about the floors mods add (docs/CONTENT_LAYER_PLAN.md).
#include <kf/lib/types.h>

namespace kf {

// Whether floor's files are held, so the game can load it. The disc's floors are not asked about.
bool mod_floor_loadable(u8 floor);

struct ModFloorEntry {
    u8 x = 0, z = 0;
};
// The cell a mod's floor names as its entrance; (0, 0) for one not held.
ModFloorEntry mod_floor_entry(u8 floor);

} // namespace kf

#endif // KF_PLATFORM_CONTENT_H
