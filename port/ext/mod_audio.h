#pragma once
// kf.audio (docs/MODDING.md): the game's sound effects and music, and a mod's own sounds (WAV files
// from its folder) mixed in beside them. Mods' sounds stop while mods are off or held.
#include <verdite/mods.h>

#include <string_view>
#include <vector>

namespace kf::ext {

std::vector<verdite::ModFunction> mod_audio_functions();

// The game's mix adds the mods' sounds to each output frame (sound_set_extra_mix), on the main thread.
void mod_audio_mix_frame(float &left, float &right);
// Stops a mod's sounds, or every mod's (mods off). Each call first mixes what has elapsed.
void mod_audio_stop_mod(std::string_view id);
void mod_audio_stop_all();
[[nodiscard]] bool mod_audio_any_playing();
// The mod's Lua state is discarded: its sounds are freed and its voices stop.
void mod_audio_forget(std::string_view id);

} // namespace kf::ext
