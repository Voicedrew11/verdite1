#include <verdite/wav.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace verdite {
namespace {

constexpr std::size_t riff_header_bytes = 12;   // "RIFF", size, "WAVE"
constexpr std::size_t chunk_header_bytes = 8;   // id, size
constexpr std::uint16_t format_pcm = 1;
constexpr std::uint32_t min_rate_hz = 8000, max_rate_hz = 96000;
constexpr std::size_t fmt_bytes = 16;           // the PCM fields; a longer fmt chunk is fine
constexpr std::int32_t unsigned_8bit_centre = 128;

struct Chunk {
    const std::uint8_t *body = nullptr;
    std::size_t size = 0;
    bool found = false;
};

struct Format {
    std::uint32_t rate_hz;
    std::uint16_t channels;
    std::uint16_t bits;
};

std::uint32_t read_u32(const std::uint8_t *at) {
    return static_cast<std::uint32_t>(at[0]) | (static_cast<std::uint32_t>(at[1]) << 8) |
           (static_cast<std::uint32_t>(at[2]) << 16) | (static_cast<std::uint32_t>(at[3]) << 24);
}

std::uint16_t read_u16(const std::uint8_t *at) {
    return static_cast<std::uint16_t>(at[0] | (at[1] << 8));
}

bool same_id(const std::uint8_t *at, const char *id) {
    return std::memcmp(at, id, 4) == 0;
}

// The first fmt and data chunks; false if a chunk's length runs past the end of the file.
bool find_chunks(std::span<const std::uint8_t> bytes, Chunk &fmt, Chunk &data) {
    std::size_t at = riff_header_bytes;
    while (at <= bytes.size() && bytes.size() - at >= chunk_header_bytes) {
        const std::uint8_t *id = bytes.data() + at;
        const std::uint32_t length = read_u32(id + 4);
        const std::size_t body = at + chunk_header_bytes;
        if (length > bytes.size() - body)
            return false;
        Chunk *wanted = same_id(id, "fmt ") ? &fmt : same_id(id, "data") ? &data : nullptr;
        if (wanted && !wanted->found)
            *wanted = {bytes.data() + body, length, true};
        // An odd chunk is padded with one byte.
        at = body + length + (length & 1u);
    }
    return true;
}

std::optional<Format> parse_format(const Chunk &fmt) {
    if (!fmt.found || fmt.size < fmt_bytes)
        return std::nullopt;
    if (read_u16(fmt.body) != format_pcm)
        return std::nullopt;
    Format format{read_u32(fmt.body + 4), read_u16(fmt.body + 2), read_u16(fmt.body + 14)};
    const std::uint16_t block_align = read_u16(fmt.body + 12);
    if (format.rate_hz < min_rate_hz || format.rate_hz > max_rate_hz)
        return std::nullopt;
    if (format.channels != 1 && format.channels != 2)
        return std::nullopt;
    if (format.bits != 8 && format.bits != 16)
        return std::nullopt;
    if (block_align != format.channels * format.bits / 8)
        return std::nullopt;
    return format;
}

// Each 8-bit sample is unsigned, centred on 128; each 16-bit sample is little-endian signed.
std::vector<std::int16_t> convert_samples(const Chunk &data, std::uint16_t bits) {
    if (bits == 8) {
        std::vector<std::int16_t> samples(data.size);
        for (std::size_t i = 0; i < data.size; ++i)
            samples[i] = static_cast<std::int16_t>((static_cast<std::int32_t>(data.body[i]) - unsigned_8bit_centre) * 256);
        return samples;
    }
    std::vector<std::int16_t> samples(data.size / 2);
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<std::int16_t>(read_u16(data.body + i * 2));
    return samples;
}

} // namespace

std::optional<Pcm> wav_decode(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < riff_header_bytes || !same_id(bytes.data(), "RIFF") || !same_id(bytes.data() + 8, "WAVE"))
        return std::nullopt;
    Chunk fmt, data;
    if (!find_chunks(bytes, fmt, data) || !data.found)
        return std::nullopt;
    const auto format = parse_format(fmt);
    if (!format)
        return std::nullopt;
    const std::size_t frame_bytes = format->channels * format->bits / 8;
    if (data.size == 0 || data.size % frame_bytes != 0)
        return std::nullopt;
    return Pcm{format->rate_hz, static_cast<std::uint8_t>(format->channels), convert_samples(data, format->bits)};
}

} // namespace verdite
