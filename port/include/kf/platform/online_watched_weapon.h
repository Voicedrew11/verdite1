#ifndef KF_PLATFORM_ONLINE_WATCHED_WEAPON_H
#define KF_PLATFORM_ONLINE_WATCHED_WEAPON_H
// PC extension (coop): the first-person weapon drawn while this player is down, which is the one
// of the player whose eyes they see through (docs/COOP_PLAN.md, "Death").
#include <kf/lib/types.h>
#include <kf/platform/online_players.h>

#include <optional>
#include <span>

namespace kf {

inline constexpr u8 online_weapon_count = 16;   // weapons are numbered 0..15 on the wire
inline constexpr s16 online_attack_phase_end = 4096;   // KF_WEAPON_ATTACK_PHASE_END

struct OnlineWatchedWeapon {
    u8 weapon;     // the watched player's equipped weapon
    u16 phase;     // its swing, 0..4095: the game's own swing phase
    u32 identity;  // what occupies the draw's slot, for frame smoothing; never 0, the game's own weapon
};

// The weapon and phase to draw: the watched player's, while they are swinging. Nothing when no one
// is watched, or the watched player has no weapon or is not swinging.
inline std::optional<OnlineWatchedWeapon> online_watched_weapon(std::span<const OnlinePlayer> players)
{
    for (const OnlinePlayer &player : players) {
        if (!player.eyes)
            continue;
        if (player.weapon >= online_weapon_count || player.attack_phase < 0 || player.attack_phase >= online_attack_phase_end)
            return std::nullopt;
        return OnlineWatchedWeapon{player.weapon, static_cast<u16>(player.attack_phase),
            1u + ((static_cast<u32>(player.id) << 4) | player.weapon)};
    }
    return std::nullopt;
}

}

#endif // KF_PLATFORM_ONLINE_WATCHED_WEAPON_H
