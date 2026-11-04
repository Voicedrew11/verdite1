#include "rated_file.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <vector>

namespace vo {
namespace {

constexpr std::size_t id_bytes = 8;

} // namespace

std::set<std::uint64_t> parse_rated_ids(std::span<const std::uint8_t> bytes) {
    std::set<std::uint64_t> ids;
    const std::size_t count = std::min(bytes.size() / id_bytes, rated_ids_most);
    for (std::size_t i = 0; i < count; ++i) {
        std::uint64_t id = 0;
        for (std::size_t b = 0; b < id_bytes; ++b)
            id |= static_cast<std::uint64_t>(bytes[i * id_bytes + b]) << (8 * b);
        ids.insert(id);
    }
    return ids;
}

std::set<std::uint64_t> read_rated_ids(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    std::vector<std::uint8_t> bytes(rated_ids_most * id_bytes);
    file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(file.gcount()));
    return parse_rated_ids(bytes);
}

bool append_rated_id(const std::string &path, std::uint64_t id) {
    std::array<char, id_bytes> bytes{};
    for (std::size_t b = 0; b < id_bytes; ++b)
        bytes[b] = static_cast<char>((id >> (8 * b)) & 0xff);
    std::ofstream file(path, std::ios::binary | std::ios::app);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    return !file.fail();
}

} // namespace vo
