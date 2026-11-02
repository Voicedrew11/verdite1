#include "../ext/mod_audio_mix.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace kf::ext;

namespace {

// The value, or a test failure: the optional is checked before it is read.
template <typename T>
T checked(const std::optional<T> &value) {
    if (!value)
        throw std::runtime_error("expected a value");
    return *value;
}

std::shared_ptr<const verdite::Pcm> pcm(std::uint32_t rate_hz, std::uint8_t channels, std::vector<std::int16_t> samples) {
    return std::make_shared<const verdite::Pcm>(verdite::Pcm{rate_hz, channels, std::move(samples)});
}

// Mixes one frame and returns it.
struct Frame {
    float left = 0, right = 0;
};
Frame mix_one(ModAudioMixer &mixer) {
    Frame frame;
    mixer.mix(frame.left, frame.right);
    return frame;
}

// Mixes until the voice ends (or a bound), returning the frames it made.
std::vector<Frame> play_out(ModAudioMixer &mixer, ModVoice voice, std::size_t bound = 1'000'000) {
    std::vector<Frame> frames;
    while (mixer.playing(voice) && frames.size() < bound)
        frames.push_back(mix_one(mixer));
    return frames;
}

// A voice that plays the sound once at full volume, centred, not looping.
ModVoice start(ModAudioMixer &mixer, std::uint32_t sound, ModVoiceOptions options = {}) {
    return checked(mixer.play("a", sound, options));
}

constexpr float near_tolerance = 1e-4f;

} // namespace

TEST_CASE("a 22050 Hz sound is resampled to 44100 Hz: twice the frames, interpolated between them") {
    ModAudioMixer mixer;
    const auto sound = checked(mixer.add_sound("a", pcm(22050, 1, {0, 16384, 0, -16384})));
    const auto frames = play_out(mixer, start(mixer, sound));
    REQUIRE(frames.size() == 8);
    const float expected[] = {0.0f, 0.25f, 0.5f, 0.25f, 0.0f, -0.25f, -0.5f, -0.5f};
    for (std::size_t i = 0; i < frames.size(); ++i)
        CHECK(std::fabs(frames[i].left - expected[i]) < near_tolerance);
}

TEST_CASE("a 44100 Hz mono sound plays one output frame per sample, on both sides") {
    ModAudioMixer mixer;
    const auto sound = checked(mixer.add_sound("a", pcm(44100, 1, {16384, -16384, 0})));
    const auto frames = play_out(mixer, start(mixer, sound));
    REQUIRE(frames.size() == 3);
    CHECK(std::fabs(frames[0].left - 0.5f) < near_tolerance);
    CHECK(std::fabs(frames[0].right - 0.5f) < near_tolerance);
    CHECK(std::fabs(frames[1].left + 0.5f) < near_tolerance);
}

TEST_CASE("a stereo sound keeps its left and right channels at centre pan") {
    ModAudioMixer mixer;
    const auto sound = checked(mixer.add_sound("a", pcm(44100, 2, {16384, -16384})));
    const auto frames = play_out(mixer, start(mixer, sound));
    REQUIRE(frames.size() == 1);
    CHECK(std::fabs(frames[0].left - 0.5f) < near_tolerance);
    CHECK(std::fabs(frames[0].right + 0.5f) < near_tolerance);
}

TEST_CASE("volume scales the voice, and pan fades the far side") {
    ModAudioMixer mixer;
    const auto sound = checked(mixer.add_sound("a", pcm(44100, 1, {32767})));
    const float full = 32767.0f / 32768.0f;
    {
        const auto frames = play_out(mixer, start(mixer, sound, {.volume = 0.5f}));
        REQUIRE(frames.size() == 1);
        CHECK(std::fabs(frames[0].left - full * 0.5f) < near_tolerance);
    }
    {
        const auto frames = play_out(mixer, start(mixer, sound, {.pan = -1.0f}));
        REQUIRE(frames.size() == 1);
        CHECK(std::fabs(frames[0].left - full) < near_tolerance);
        CHECK(frames[0].right == 0.0f);
    }
    {
        const auto frames = play_out(mixer, start(mixer, sound, {.pan = 0.5f}));
        REQUIRE(frames.size() == 1);
        CHECK(std::fabs(frames[0].left - full * 0.5f) < near_tolerance);
        CHECK(std::fabs(frames[0].right - full) < near_tolerance);
    }
}

TEST_CASE("a looping sound goes on until stopped, and repeats") {
    ModAudioMixer mixer;
    const auto sound = checked(mixer.add_sound("a", pcm(44100, 1, {16384, -16384})));
    const auto voice = start(mixer, sound, {.loop = true});
    const auto frames = play_out(mixer, voice, 10);
    REQUIRE(frames.size() == 10);
    CHECK(mixer.playing(voice));
    CHECK(std::fabs(frames[0].left - frames[2].left) < near_tolerance);
    mixer.stop(voice);
    CHECK_FALSE(mixer.playing(voice));
}

TEST_CASE("at most 16 voices play across all mods, and a stopped one frees its slot") {
    ModAudioMixer mixer;
    const auto sound = checked(mixer.add_sound("a", pcm(44100, 1, {1, 1, 1, 1})));
    const auto other = checked(mixer.add_sound("b", pcm(44100, 1, {1})));
    std::vector<ModVoice> voices;
    voices.reserve(mod_audio_voice_count);
    for (std::size_t i = 0; i < mod_audio_voice_count; ++i)
        voices.push_back(start(mixer, sound));
    CHECK_FALSE(mixer.play("b", other, {}).has_value());
    CHECK_FALSE(mixer.play("a", sound, {}).has_value());
    mixer.stop(voices.front());
    CHECK(mixer.play("b", other, {}).has_value());
}

TEST_CASE("a stale voice id matches nothing, even when its slot plays again") {
    ModAudioMixer mixer;
    const auto sound = checked(mixer.add_sound("a", pcm(44100, 1, {1, 1, 1, 1, 1, 1, 1, 1})));
    const auto first = start(mixer, sound);
    mixer.stop(first);
    CHECK_FALSE(mixer.playing(first));
    const auto second = start(mixer, sound);
    CHECK(second.slot == first.slot);
    CHECK_FALSE(mixer.playing(first));
    mixer.stop(first);
    CHECK(mixer.playing(second));
    CHECK_FALSE(mixer.playing(ModVoice{99, second.generation}));
}

TEST_CASE("a voice that has ended frees its slot and its id goes stale") {
    ModAudioMixer mixer;
    const auto sound = checked(mixer.add_sound("a", pcm(44100, 1, {1})));
    const auto voice = start(mixer, sound);
    mix_one(mixer);
    CHECK_FALSE(mixer.playing(voice));
    CHECK_FALSE(mixer.any_playing());
}

TEST_CASE("a sound is the mod's own: another mod cannot play its id") {
    ModAudioMixer mixer;
    const auto sound = checked(mixer.add_sound("a", pcm(44100, 1, {1})));
    CHECK(mixer.has_sound("a", sound));
    CHECK_FALSE(mixer.has_sound("b", sound));
    CHECK_FALSE(mixer.play("b", sound, {}).has_value());
    CHECK_FALSE(mixer.has_sound("a", sound + 1000));
}

TEST_CASE("a mod's sounds are capped at 64 MB of decoded samples, and a sound over the cap is refused") {
    ModAudioMixer mixer;
    const std::size_t half_cap_samples = (mod_audio_sound_bytes_cap / 2) / 2;
    CHECK(mixer.add_sound("a", pcm(44100, 1, std::vector<std::int16_t>(half_cap_samples))).has_value());
    CHECK(mixer.add_sound("a", pcm(44100, 1, std::vector<std::int16_t>(half_cap_samples))).has_value());
    CHECK_FALSE(mixer.add_sound("a", pcm(44100, 1, {1})).has_value());
    CHECK(mixer.add_sound("b", pcm(44100, 1, {1})).has_value());
}

TEST_CASE("a sound with no frames, or a rate of 0, is refused") {
    ModAudioMixer mixer;
    CHECK_FALSE(mixer.add_sound("a", pcm(44100, 1, {})).has_value());
    CHECK_FALSE(mixer.add_sound("a", pcm(0, 1, {1})).has_value());
    CHECK_FALSE(mixer.add_sound("a", nullptr).has_value());
}

TEST_CASE("forgetting a mod stops its voices and drops its sounds; other mods play on") {
    ModAudioMixer mixer;
    const auto sound = checked(mixer.add_sound("a", pcm(44100, 1, {1, 1, 1, 1})));
    const auto other = checked(mixer.add_sound("b", pcm(44100, 1, {1, 1, 1, 1})));
    const auto gone = start(mixer, sound);
    const auto kept = checked(mixer.play("b", other, {}));
    mixer.forget("a");
    CHECK_FALSE(mixer.playing(gone));
    CHECK_FALSE(mixer.has_sound("a", sound));
    CHECK(mixer.playing(kept));
    CHECK_FALSE(mixer.play("a", sound, {}).has_value());
}

TEST_CASE("stop_mod stops only that mod's voices, and stop_all stops every voice") {
    ModAudioMixer mixer;
    const auto sound = checked(mixer.add_sound("a", pcm(44100, 1, {1, 1, 1, 1})));
    const auto other = checked(mixer.add_sound("b", pcm(44100, 1, {1, 1, 1, 1})));
    const auto a = start(mixer, sound);
    const auto b = checked(mixer.play("b", other, {}));
    mixer.stop_mod("a");
    CHECK_FALSE(mixer.playing(a));
    CHECK(mixer.playing(b));
    mixer.stop_all();
    CHECK_FALSE(mixer.playing(b));
    CHECK_FALSE(mixer.any_playing());
}

TEST_CASE("the output stays finite with every voice loud, and with a volume or pan that is not finite") {
    ModAudioMixer mixer;
    const auto sound = checked(mixer.add_sound("a", pcm(96000, 2, std::vector<std::int16_t>(4096, 32767))));
    for (std::size_t i = 0; i < mod_audio_voice_count; ++i)
        start(mixer, sound, {.volume = 1.0f, .pan = static_cast<float>(i % 3) - 1.0f, .loop = true});
    for (int i = 0; i < 2000; ++i) {
        const Frame frame = mix_one(mixer);
        CHECK(std::isfinite(frame.left));
        CHECK(std::isfinite(frame.right));
        CHECK(std::fabs(frame.left) <= static_cast<float>(mod_audio_voice_count));
    }
    mixer.stop_all();
    const auto silent = start(mixer, sound, {.volume = NAN, .pan = NAN});
    CHECK(mixer.playing(silent));
    const Frame frame = mix_one(mixer);
    CHECK(frame.left == 0.0f);
    CHECK(frame.right == 0.0f);
}
