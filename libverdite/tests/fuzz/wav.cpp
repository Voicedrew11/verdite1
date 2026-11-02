// libFuzzer: a WAV file from a mod's folder. Decoding must not crash, and what it returns must be
// a whole number of frames at a channel count and rate the game can play.
#include <verdite/wav.h>

#include <cstdlib>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const auto pcm = verdite::wav_decode(std::span(data, size));
    if (!pcm)
        return 0;
    const bool channels_ok = pcm->channels == 1 || pcm->channels == 2;
    const bool rate_ok = pcm->rate_hz >= 8000 && pcm->rate_hz <= 96000;
    if (!channels_ok || !rate_ok || pcm->samples.empty() || pcm->samples.size() % pcm->channels != 0)
        std::abort();
    return 0;
}
