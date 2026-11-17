#pragma once
// The disc files a mod may replace with its assets/ folder (docs/MODDING.md): the kinds the game can
// load and the check each file must pass, the disc's own size of a file, and the replacement the
// game's next load of a path takes.
#include <verdite/mods.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kf::ext {

// .TIM and .SEQ, each with the game's own parse as its check; .TMD with the checks of what a draw
// reads. MIM is left out: see mod_assets.cpp.
std::vector<verdite::ModAssetKind> mod_asset_kinds();
// The same checks as the kinds' rows, for a floor's files (mod_floor_check.h). The TMD's has no kind's words around it.
std::optional<std::string> mod_tim_refusal(std::span<const std::uint8_t> bytes);
std::optional<std::string> mod_seq_refusal(std::span<const std::uint8_t> bytes);
std::optional<std::string> mod_tmd_refusal(std::span<const std::uint8_t> tmd);
// The size of the disc's file at a disc path ("KF/B1/SND0.SEQ"), read from the disc: nothing if none.
std::optional<std::size_t> mod_disc_file_size(std::string_view path);
// Pictures inside a file of several TIMs, named "<file>#<n>" (n from 0): the opening's and ending's
// scroll pages and the notification banners. A replacement is checked against the disc's picture and
// put where it was, so a mod need not know where the game keeps it.
//
// The picture paths a mod may replace inside a file ("KF/B0/MIX3."); none for another file.
std::vector<std::string_view> mod_pictures_in(std::string_view file);
// What a mod has set for a picture path, or null.
using PictureLookup = std::function<std::shared_ptr<const std::vector<std::uint8_t>>(std::string_view path)>;
// The file with each picture a mod replaced put in, from the disc's file; nothing if no mod replaced
// any of its pictures (or the disc's file is not a file of TIMs).
std::optional<std::vector<std::uint8_t>> mod_pictures_put_in(std::string_view file, std::span<const std::uint8_t> disc,
                                                             const PictureLookup &replacement_of);
// One picture as one TIM to upload on its own: the replacement where the disc's picture goes, or the
// disc's picture when replacement is null; nothing if the disc's file has no such picture.
std::optional<std::vector<std::uint8_t>> mod_picture_for_upload(std::string_view path, std::span<const std::uint8_t> disc,
                                                                 const std::vector<std::uint8_t> *replacement);
// The disc's file at path in a folder (a language's), never a mod's replacement; nothing if none.
std::optional<std::vector<std::uint8_t>> mod_disc_file_at(const char *directory, std::string_view path);
// The bytes a mod replaces path with, or null for the disc's (defined in mods.cpp, with the host).
// directory: the folder the disc's file is opened from, as for kf::DataReplacer.
std::shared_ptr<const std::vector<std::uint8_t>> mod_asset_for_path(const char *path, const char *directory);

} // namespace kf::ext
