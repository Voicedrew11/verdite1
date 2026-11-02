#pragma once
// The mixing of mods' sounds (kf.audio, docs/MODDING.md): each mod's decoded sounds, and 16 voices
// shared by all mods. A voice plays one sound, resampled linearly to the output rate, with a volume,
// a pan and a loop. No Lua and no game here: mod_audio.cpp binds it, and the game's mix calls it.
#include <verdite/wav.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace kf::ext {

inline constexpr std::size_t mod_audio_voice_count = 16;
inline constexpr std::size_t mod_audio_sound_bytes_cap = 64u << 20;   // per mod, of decoded 16-bit samples
inline constexpr std::uint32_t mod_audio_output_rate_hz = 44100;

// A voice as a mod holds it. The generation changes each time the slot is played again, so a voice
// that has ended matches nothing in its slot.
struct ModVoice {
    std::uint32_t slot = 0;
    std::uint32_t generation = 0;
};

struct ModVoiceOptions {
    float volume = 1.0f;   // 0 to 1; anything not finite plays silent
    float pan = 0.0f;      // -1 (left) to 1 (right); anything not finite centres
    bool loop = false;
};

class ModAudioMixer {
public:
    // The sound's id; nothing if the mod's sounds would pass mod_audio_sound_bytes_cap, or the
    // sound has no frames.
    [[nodiscard]] std::optional<std::uint32_t> add_sound(std::string_view mod, std::shared_ptr<const verdite::Pcm> pcm);
    // Whether the mod has a sound with this id.
    [[nodiscard]] bool has_sound(std::string_view mod, std::uint32_t sound) const;
    // A voice playing the mod's sound; nothing if all voices are busy or the sound is not the mod's.
    [[nodiscard]] std::optional<ModVoice> play(std::string_view mod, std::uint32_t sound, const ModVoiceOptions &options);
    // Stops a voice; one that has ended, or was stopped, is left alone.
    void stop(ModVoice voice);
    [[nodiscard]] bool playing(ModVoice voice) const;
    // Stops the mod's voices, and drops its sounds and their memory.
    void forget(std::string_view mod);
    // Stops the voices of one mod, or of all mods.
    void stop_mod(std::string_view mod);
    void stop_all();
    [[nodiscard]] bool any_playing() const;
    // Adds one output frame of every playing voice to left and right.
    void mix(float &left, float &right);

private:
    struct Voice {
        bool active = false;
        std::uint32_t generation = 0;
        std::string mod;
        std::shared_ptr<const verdite::Pcm> pcm;   // held only while the voice plays
        ModVoiceOptions options;
        double position = 0;   // in the sound's frames
        double step = 0;       // the sound's frames per output frame
    };
    struct ModSounds {
        std::map<std::uint32_t, std::shared_ptr<const verdite::Pcm>> by_id;
        std::size_t bytes = 0;   // of decoded samples
    };
    [[nodiscard]] const std::shared_ptr<const verdite::Pcm> *find_sound(std::string_view mod, std::uint32_t sound) const;
    // The slot of the voice if it is still playing as it was started, else mod_audio_voice_count.
    [[nodiscard]] std::size_t live_slot(ModVoice voice) const;
    static void end(Voice &voice);
    std::map<std::string, ModSounds, std::less<>> mods_;
    std::array<Voice, mod_audio_voice_count> voices_{};
    std::uint32_t last_sound_ = 0;
};

} // namespace kf::ext
