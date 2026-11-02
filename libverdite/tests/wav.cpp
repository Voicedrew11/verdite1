#include <verdite/wav.h>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

using namespace verdite;

namespace {
using Bytes = std::vector<std::uint8_t>;

void put_u32(Bytes &out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}

void put_u16(Bytes &out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void put_tag(Bytes &out, const char *tag) {
    out.insert(out.end(), tag, tag + 4);
}

// A chunk with its length as given, then its body.
void put_chunk(Bytes &out, const char *tag, std::uint32_t length, const Bytes &body) {
    put_tag(out, tag);
    put_u32(out, length);
    out.insert(out.end(), body.begin(), body.end());
}

// The 16-byte PCM fmt body; block_align follows the channels and depth unless given.
Bytes fmt_body(std::uint16_t format, std::uint16_t channels, std::uint32_t rate, std::uint16_t bits,
               int block_align = -1) {
    Bytes body;
    put_u16(body, format);
    put_u16(body, channels);
    put_u32(body, rate);
    put_u32(body, rate * channels * bits / 8);
    put_u16(body, static_cast<std::uint16_t>(block_align < 0 ? channels * bits / 8 : block_align));
    put_u16(body, bits);
    return body;
}

// A RIFF/WAVE file: the chunks in order, each as put_chunk writes them.
Bytes riff(const Bytes &chunks) {
    Bytes out;
    put_tag(out, "RIFF");
    put_u32(out, static_cast<std::uint32_t>(4 + chunks.size()));
    put_tag(out, "WAVE");
    out.insert(out.end(), chunks.begin(), chunks.end());
    return out;
}

Bytes wav(std::uint16_t format, std::uint16_t channels, std::uint32_t rate, std::uint16_t bits, const Bytes &data,
          int block_align = -1) {
    Bytes chunks;
    const Bytes fmt = fmt_body(format, channels, rate, bits, block_align);
    put_chunk(chunks, "fmt ", static_cast<std::uint32_t>(fmt.size()), fmt);
    put_chunk(chunks, "data", static_cast<std::uint32_t>(data.size()), data);
    return riff(chunks);
}

std::optional<Pcm> decode(const Bytes &bytes) {
    return wav_decode(std::span<const std::uint8_t>(bytes));
}

// The decoded file, or a test failure: the optional is checked before it is read.
Pcm checked(const std::optional<Pcm> &pcm) {
    if (!pcm)
        throw std::runtime_error("expected a decoded file");
    return *pcm;
}
} // namespace

TEST_CASE("a 16-bit mono file decodes to its samples, little-endian signed") {
    const auto pcm = checked(decode(wav(1, 1, 22050, 16, {0x00, 0x80, 0xff, 0x7f, 0x34, 0x12})));
    CHECK(pcm.rate_hz == 22050);
    CHECK(pcm.channels == 1);
    CHECK(pcm.samples == std::vector<std::int16_t>{-32768, 32767, 0x1234});
}

TEST_CASE("an 8-bit stereo file decodes to 16-bit interleaved samples, centred on zero") {
    const auto pcm = checked(decode(wav(1, 2, 44100, 8, {128, 255, 0, 128})));
    CHECK(pcm.channels == 2);
    CHECK(pcm.samples == std::vector<std::int16_t>{0, 32512, -32768, 0});
}

TEST_CASE("the rate bounds 8000 and 96000 are accepted") {
    CHECK(decode(wav(1, 1, 8000, 8, {128})).has_value());
    CHECK(decode(wav(1, 1, 96000, 8, {128})).has_value());
}

TEST_CASE("a file that is not PCM at 8 or 16 bits, 1 or 2 channels, and 8000 to 96000 Hz is refused") {
    const Bytes data = {0, 0, 0, 0, 0, 0};
    CHECK_FALSE(decode(wav(3, 1, 22050, 16, data)).has_value());        // IEEE float
    CHECK_FALSE(decode(wav(0xfffe, 1, 22050, 16, data)).has_value());   // extensible
    CHECK_FALSE(decode(wav(1, 1, 22050, 24, data)).has_value());        // 24-bit
    CHECK_FALSE(decode(wav(1, 3, 22050, 8, data)).has_value());         // three channels
    CHECK_FALSE(decode(wav(1, 0, 22050, 8, data)).has_value());         // no channels
    CHECK_FALSE(decode(wav(1, 1, 7999, 8, data)).has_value());
    CHECK_FALSE(decode(wav(1, 1, 96001, 8, data)).has_value());
    CHECK_FALSE(decode(wav(1, 1, 0, 8, data)).has_value());
}

TEST_CASE("a block align that does not match the frame is refused") {
    CHECK_FALSE(decode(wav(1, 2, 22050, 16, {0, 0, 0, 0}, 2)).has_value());
}

TEST_CASE("an empty data chunk, or one not a whole number of frames, is refused") {
    CHECK_FALSE(decode(wav(1, 1, 22050, 16, {})).has_value());
    CHECK_FALSE(decode(wav(1, 1, 22050, 16, {1, 2, 3})).has_value());
    CHECK_FALSE(decode(wav(1, 2, 22050, 16, {1, 2, 3, 4, 5, 6})).has_value());
}

TEST_CASE("a file without a fmt or a data chunk, or not RIFF/WAVE, is refused") {
    Bytes chunks;
    put_chunk(chunks, "data", 2, {1, 2});
    CHECK_FALSE(decode(riff(chunks)).has_value());
    Bytes fmt_only;
    const Bytes fmt = fmt_body(1, 1, 22050, 8);
    put_chunk(fmt_only, "fmt ", 16, fmt);
    CHECK_FALSE(decode(riff(fmt_only)).has_value());
    Bytes bytes = wav(1, 1, 22050, 8, {128});
    bytes[0] = 'X';
    CHECK_FALSE(decode(bytes).has_value());
    bytes = wav(1, 1, 22050, 8, {128});
    bytes[8] = 'X';
    CHECK_FALSE(decode(bytes).has_value());
    CHECK_FALSE(decode(Bytes{'R', 'I', 'F', 'F'}).has_value());
}

TEST_CASE("a fmt chunk shorter than the PCM fields is refused") {
    Bytes chunks;
    const Bytes fmt = fmt_body(1, 1, 22050, 8);
    put_chunk(chunks, "fmt ", 14, Bytes(fmt.begin(), fmt.begin() + 14));
    put_chunk(chunks, "data", 1, {128});
    CHECK_FALSE(decode(riff(chunks)).has_value());
}

TEST_CASE("a chunk length of 0xFFFFFFFF, or one running past the end, is refused") {
    // The data chunk is last: its length sits four bytes before its one-byte body.
    Bytes huge = wav(1, 1, 22050, 8, {128});
    const std::size_t data_length_at = huge.size() - 1 - 4;
    huge[data_length_at] = huge[data_length_at + 1] = huge[data_length_at + 2] = huge[data_length_at + 3] = 0xff;
    CHECK_FALSE(decode(huge).has_value());
    Bytes past = wav(1, 1, 22050, 8, {128, 129});
    past[past.size() - 2 - 4] = 3;   // one byte more than the two the file holds
    CHECK_FALSE(decode(past).has_value());
}

TEST_CASE("an odd chunk before the data is padded by one byte and skipped") {
    Bytes chunks;
    put_chunk(chunks, "LIST", 3, {'a', 'b', 'c', 0});
    const Bytes fmt = fmt_body(1, 1, 22050, 8);
    put_chunk(chunks, "fmt ", 16, fmt);
    put_chunk(chunks, "data", 2, {128, 255});
    const auto pcm = checked(decode(riff(chunks)));
    CHECK(pcm.samples == std::vector<std::int16_t>{0, 32512});
}

TEST_CASE("truncation at every byte never crashes, and only the whole file decodes") {
    const Bytes whole = wav(1, 2, 22050, 16, {1, 0, 2, 0, 3, 0, 4, 0});
    for (std::size_t length = 0; length < whole.size(); ++length) {
        const Bytes cut(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(length));
        CHECK_FALSE(decode(cut).has_value());
    }
    CHECK(decode(whole).has_value());
}
