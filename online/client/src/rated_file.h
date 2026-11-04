#ifndef VERDITE_ONLINE_RATED_FILE_H
#define VERDITE_ONLINE_RATED_FILE_H
// The records this install has rated, kept beside its key so a rating is not offered again in a later
// session: each id's 8 bytes, little-endian, one after another. A file that is missing or cut short
// loses only what it no longer holds, and the service still counts one rating per rater.
#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <string>

namespace vo {

// The most ids a file is read for; past it, a rating may be offered again.
inline constexpr std::size_t rated_ids_most = 1u << 16;

// The ids in a file's bytes, leaving out a last one cut short and any past rated_ids_most.
std::set<std::uint64_t> parse_rated_ids(std::span<const std::uint8_t> bytes);
std::set<std::uint64_t> read_rated_ids(const std::string &path);
// False when it could not be written.
bool append_rated_id(const std::string &path, std::uint64_t id);

} // namespace vo

#endif // VERDITE_ONLINE_RATED_FILE_H
