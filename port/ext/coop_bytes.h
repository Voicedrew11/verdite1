#pragma once
// Little-endian bytes for the coop world file and the coop messages: a writer that appends, and a
// reader that fails once anything runs past the end and stays failed.
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kf::ext {

class CoopByteWriter {
public:
    void u8(std::uint8_t value) { bytes_.push_back(value); }
    void u16(std::uint16_t value) { unsigned_le(value, 2); }
    void u32(std::uint32_t value) { unsigned_le(value, 4); }
    void u64(std::uint64_t value) { unsigned_le(value, 8); }
    void s32(std::int32_t value) { u32(static_cast<std::uint32_t>(value)); }
    void raw(std::span<const std::uint8_t> bytes) { bytes_.insert(bytes_.end(), bytes.begin(), bytes.end()); }
    // A length (u8 for a name, u32 for a blob) and the bytes.
    void name(std::string_view text) {
        u8(static_cast<std::uint8_t>(text.size()));
        bytes_.insert(bytes_.end(), text.begin(), text.end());
    }
    void blob(std::span<const std::uint8_t> bytes) {
        u32(static_cast<std::uint32_t>(bytes.size()));
        raw(bytes);
    }
    std::vector<std::uint8_t> &bytes() { return bytes_; }

private:
    void unsigned_le(std::uint64_t value, int count) {
        for (int i = 0; i < count; ++i)
            bytes_.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
    std::vector<std::uint8_t> bytes_;
};

class CoopByteReader {
public:
    explicit CoopByteReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    std::uint8_t u8() { return static_cast<std::uint8_t>(unsigned_le(1)); }
    std::uint16_t u16() { return static_cast<std::uint16_t>(unsigned_le(2)); }
    std::uint32_t u32() { return static_cast<std::uint32_t>(unsigned_le(4)); }
    std::uint64_t u64() { return unsigned_le(8); }
    std::int32_t s32() { return static_cast<std::int32_t>(u32()); }
    std::span<const std::uint8_t> raw(std::size_t count) {
        if (!ok_ || count > bytes_.size() - at_) {
            ok_ = false;
            return {};
        }
        const auto taken = bytes_.subspan(at_, count);
        at_ += count;
        return taken;
    }
    std::string name() {
        const auto taken = raw(u8());
        return {taken.begin(), taken.end()};
    }
    std::vector<std::uint8_t> blob() {
        const auto taken = raw(u32());
        return {taken.begin(), taken.end()};
    }
    // Whether everything read so far was there.
    bool ok() const { return ok_; }
    // Whether everything read was there and nothing is left over.
    bool done() const { return ok_ && at_ == bytes_.size(); }

private:
    std::uint64_t unsigned_le(int count) {
        const auto taken = raw(static_cast<std::size_t>(count));
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < taken.size(); ++i)
            value |= static_cast<std::uint64_t>(taken[i]) << (8 * i);
        return value;
    }
    std::span<const std::uint8_t> bytes_;
    std::size_t at_ = 0;
    bool ok_ = true;
};

}
