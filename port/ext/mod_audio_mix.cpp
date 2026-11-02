#include "mod_audio_mix.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace kf::ext {
namespace {

constexpr float sample_scale = 32768.0f;
constexpr std::size_t bytes_per_sample = sizeof(std::int16_t);

std::size_t frames_of(const verdite::Pcm &pcm) {
    return pcm.samples.size() / pcm.channels;
}

// A value from outside: anything not finite takes the fallback.
float clamped(float value, float low, float high, float fallback) {
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}

ModVoiceOptions sanitized(ModVoiceOptions options) {
    options.volume = clamped(options.volume, 0.0f, 1.0f, 0.0f);
    options.pan = clamped(options.pan, -1.0f, 1.0f, 0.0f);
    return options;
}

// One channel's sample at a frame, as -1 to 1; a mono sound is its one channel for both sides.
float sample_at(const verdite::Pcm &pcm, std::size_t frame, unsigned channel) {
    const std::size_t index = frame * pcm.channels + (pcm.channels == 2 ? channel : 0);
    return pcm.samples[index] / sample_scale;
}

// Left and right at a position in frames (before the frames run out), linear between the two
// frames around it; a looping sound's last frame is followed by its first.
std::array<float, 2> source_at(const verdite::Pcm &pcm, double position, bool loop) {
    const std::size_t frames = frames_of(pcm);
    const auto whole = static_cast<std::size_t>(position);
    const auto fraction = static_cast<float>(position - static_cast<double>(whole));
    std::size_t next = whole + 1;
    if (next >= frames)
        next = loop ? 0 : whole;
    std::array<float, 2> out{};
    for (unsigned channel = 0; channel < out.size(); ++channel) {
        const float from = sample_at(pcm, whole, channel);
        const float to = sample_at(pcm, next, channel);
        out[channel] = from + (to - from) * fraction;
    }
    return out;
}

// The voice's left and right for this output frame: volume, then a balance pan (the far side fades).
std::array<float, 2> output_of(const verdite::Pcm &pcm, double position, const ModVoiceOptions &options) {
    const auto source = source_at(pcm, position, options.loop);
    const float left_gain = options.volume * std::min(1.0f, 1.0f - options.pan);
    const float right_gain = options.volume * std::min(1.0f, 1.0f + options.pan);
    return {source[0] * left_gain, source[1] * right_gain};
}

} // namespace

void ModAudioMixer::end(Voice &voice) {
    voice.active = false;
    voice.mod.clear();
    voice.pcm.reset();
}

std::optional<std::uint32_t> ModAudioMixer::add_sound(std::string_view mod, std::shared_ptr<const verdite::Pcm> pcm) {
    if (!pcm || frames_of(*pcm) == 0 || pcm->rate_hz == 0)
        return std::nullopt;
    auto found = mods_.find(mod);
    if (found == mods_.end())
        found = mods_.emplace(std::string(mod), ModSounds{}).first;
    const std::size_t bytes = pcm->samples.size() * bytes_per_sample;
    if (bytes > mod_audio_sound_bytes_cap - found->second.bytes)
        return std::nullopt;
    if (++last_sound_ == 0)
        ++last_sound_;
    found->second.bytes += bytes;
    found->second.by_id.emplace(last_sound_, std::move(pcm));
    return last_sound_;
}

const std::shared_ptr<const verdite::Pcm> *ModAudioMixer::find_sound(std::string_view mod, std::uint32_t sound) const {
    const auto found = mods_.find(mod);
    if (found == mods_.end())
        return nullptr;
    const auto sounds = found->second.by_id.find(sound);
    return sounds == found->second.by_id.end() ? nullptr : &sounds->second;
}

bool ModAudioMixer::has_sound(std::string_view mod, std::uint32_t sound) const {
    return find_sound(mod, sound) != nullptr;
}

std::optional<ModVoice> ModAudioMixer::play(std::string_view mod, std::uint32_t sound, const ModVoiceOptions &options) {
    const auto *pcm = find_sound(mod, sound);
    if (!pcm)
        return std::nullopt;
    for (std::uint32_t slot = 0; slot < mod_audio_voice_count; ++slot) {
        Voice &voice = voices_[slot];
        if (voice.active)
            continue;
        voice.generation = voice.generation + 1 == 0 ? 1 : voice.generation + 1;   // never 0
        voice.active = true;
        voice.mod = std::string(mod);
        voice.pcm = *pcm;
        voice.options = sanitized(options);
        voice.position = 0;
        voice.step = static_cast<double>((*pcm)->rate_hz) / mod_audio_output_rate_hz;
        return ModVoice{slot, voice.generation};
    }
    return std::nullopt;
}

std::size_t ModAudioMixer::live_slot(ModVoice voice) const {
    if (voice.slot >= mod_audio_voice_count)
        return mod_audio_voice_count;
    const Voice &found = voices_[voice.slot];
    return found.active && found.generation == voice.generation ? voice.slot : mod_audio_voice_count;
}

void ModAudioMixer::stop(ModVoice voice) {
    const std::size_t slot = live_slot(voice);
    if (slot < mod_audio_voice_count)
        end(voices_[slot]);
}

bool ModAudioMixer::playing(ModVoice voice) const {
    return live_slot(voice) < mod_audio_voice_count;
}

void ModAudioMixer::stop_mod(std::string_view mod) {
    for (Voice &voice : voices_)
        if (voice.active && voice.mod == mod)
            end(voice);
}

void ModAudioMixer::forget(std::string_view mod) {
    stop_mod(mod);
    if (const auto found = mods_.find(mod); found != mods_.end())
        mods_.erase(found);
}

void ModAudioMixer::stop_all() {
    for (Voice &voice : voices_)
        end(voice);
}

bool ModAudioMixer::any_playing() const {
    return std::any_of(voices_.begin(), voices_.end(), [](const Voice &voice) { return voice.active; });
}

void ModAudioMixer::mix(float &left, float &right) {
    for (Voice &voice : voices_) {
        if (!voice.active)
            continue;
        const auto out = output_of(*voice.pcm, voice.position, voice.options);
        left += out[0];
        right += out[1];
        voice.position += voice.step;
        if (voice.position < static_cast<double>(frames_of(*voice.pcm)))
            continue;
        if (voice.options.loop)
            voice.position = std::fmod(voice.position, static_cast<double>(frames_of(*voice.pcm)));
        else
            end(voice);
    }
}

} // namespace kf::ext
