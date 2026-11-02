#pragma once
// The disc files a mod may replace with its assets/ folder (docs/MODDING.md): the kinds the game can
// load and the check each file must pass, the disc's own size of a file, and the replacement the
// game's next load of a path takes.
#include <verdite/mods.h>

#include <cstddef>
#include <cstdint>
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
// The bytes a mod replaces path with, or null for the disc's (defined in mods.cpp, with the host).
std::shared_ptr<const std::vector<std::uint8_t>> mod_asset_for_path(const char *path);

} // namespace kf::ext
