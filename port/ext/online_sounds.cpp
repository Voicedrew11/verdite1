#include "online_sounds.h"

#include <kf/game/audio.h>
#include <kf/game/player.h>
#include <kf/game/player_motion.h>
#include <kf/game/state.h>
#include <kf/lib/audio.h>
#include <kf/lib/map_types.h>
#include <kf/platform/online_players.h>

#include <cmath>
#include <cstddef>

namespace kf::ext {
namespace {

static_assert(online_sound_kinds == online_sound_count);

struct KnightSound {
    const SoundRef *sound;
    s16 volume;
};

constexpr s16 key_unlock_volume = 110;   // PLAYER_KEY_UNLOCK_VOLUME

// In kf::OnlineSound's order, as the game plays each for its own player.
const std::array<KnightSound, online_sound_kinds> knight_sounds = {{
    {&player_sound_refs[KF_PLAYER_SOUND_WEAPON_ATTACK], KF_AUDIO_MAX_VOLUME},
    {&player_sound_refs[KF_PLAYER_SOUND_DEATH], KF_AUDIO_MAX_VOLUME},
    {&gameplay_sound_refs[KF_GAMEPLAY_SOUND_KEY_UNLOCK], key_unlock_volume},
    {&gameplay_sound_refs[KF_GAMEPLAY_SOUND_STONE_PASSAGE], KF_AUDIO_MAX_VOLUME},
    {&gameplay_sound_refs[KF_GAMEPLAY_SOUND_HARP], KF_AUDIO_MAX_VOLUME},
    {&gameplay_sound_refs[KF_GAMEPLAY_SOUND_WARP_SHIMMER], KF_AUDIO_MAX_VOLUME},
}};

struct Sounds {
    std::array<std::uint8_t, online_sound_kinds> made{};
    std::array<std::uint32_t, online_sound_kinds> heard{};
};
Sounds sounds;

}

std::array<std::uint8_t, online_sound_kinds> online_sounds_made() {
    return sounds.made;
}

void online_sounds_play(const OnlinePose &was, const OnlinePose &now) {
    constexpr float tile_size = KF_MAP_TILE_SIZE;
    // Where their head is, as this player's camera is.
    const VECTOR at{static_cast<s32>(std::lround(now.x * tile_size)), now.foot_height - KF_PLAYER_CAMERA_HEIGHT,
        static_cast<s32>(std::lround(now.z * tile_size))};
    for (const std::size_t sound : online_pose_sounds_between(was, now)) {
        audio_play_spatial_default_range(knight_sounds[sound].sound, &at, knight_sounds[sound].volume);
        ++sounds.heard[sound];
    }
}

std::array<std::uint32_t, online_sound_kinds> online_sounds_heard() {
    return sounds.heard;
}

}

namespace kf {

void online_sound_made(OnlineSound sound) {
    ++ext::sounds.made[static_cast<std::size_t>(sound)];
}

}
