#ifndef VERDITE_WAV_H
#define VERDITE_WAV_H
// A WAV file a mod may play: RIFF/WAVE, PCM at 8 or 16 bits, 1 or 2 channels, 8000 to 96000 Hz.
// Game-agnostic: the bytes come from the caller, and the samples are 16-bit whatever the file held.
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace verdite {

struct Pcm {
    std::uint32_t rate_hz = 0;
    std::uint8_t channels = 0;
    std::vector<std::int16_t> samples;   // interleaved; 8-bit input is converted to 16-bit
};

// Nothing for any other file: a format other than PCM, another depth, a chunk running past the end,
// or a data chunk that is empty or not a whole number of frames.
std::optional<Pcm> wav_decode(std::span<const std::uint8_t> bytes);

} // namespace verdite

#endif // VERDITE_WAV_H
