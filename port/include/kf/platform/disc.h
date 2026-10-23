#ifndef KF_PLATFORM_DISC_H
#define KF_PLATFORM_DISC_H

#include <kf/platform/assets.h>
#include <kf/platform/language.h>

#include <span>

namespace kf {
// The supported retail CUE is one MODE2/2352 track at file offset zero.
bool disc_cue_image(const char *text, std::span<char> filename);
// Verify and prepare the selected language in a new directory. Never overwrite.
bool disc_extract(const char *source, const char *destination, Language language);
bool disc_prepare_directory(const char *source, const char *destination, Language language);
// Startup-only verification also prevents a --data tree from mixing languages.
// With actual supplied, English startup may also accept a Japanese base to convert.
bool disc_verify_directory(const char *directory, Language language, Language *actual = nullptr);
// PC extensions (Windows release flow, any supported language):
// Reads every game file of a disc image into memory and says which release it is.
bool disc_import_image(const char *source, AssetTable &assets, DiscMatch *match);
// Writes the files into a new directory (never over an existing one).
bool resource_tree_write(const AssetTable &assets, const char *destination);
// Reads the retail file list back from an extracted directory.
bool resource_tree_read(const char *directory, AssetTable &assets);
}

#endif // KF_PLATFORM_DISC_H
