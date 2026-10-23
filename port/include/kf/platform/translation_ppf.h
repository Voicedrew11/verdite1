#ifndef KF_PLATFORM_TRANSLATION_PPF_H
#define KF_PLATFORM_TRANSLATION_PPF_H
#include <kf/platform/assets.h>
#include <string>
#include <vector>

namespace kf {
// John Osborne's (weissvulf) English translation v1.0 is a PPF 3.0 patch for the
// Japanese disc image. Nothing translated ships with the port: players either
// supply an image with the patch applied, or the patch itself, which is applied
// to their own Japanese files and checked against the known English result.
// The translator's release, as the Wayback Machine archived their site.
inline constexpr const char *english_patch_url =
    "https://web.archive.org/web/20211128030012id_/https://www.angelfire.com/art3/weissvulf/Kings_Field_Jap_to_Eng_v1.0.rar";
inline constexpr const char *english_patch_archive_sha256 =
    "d4131e4a2e3e9f3c0188e4c2c6c2eca632dc5a954da8db4af299bc4fbe2edb6e";

// Applies a PPF 3.0 disc-image patch to the Japanese files in memory (through
// the retail disc layout) and requires the English v1.0 result. On failure the
// table must be discarded.
bool translation_apply_ppf(AssetTable &assets, const std::vector<u8> &ppf, std::string *error);
// Reads a patch from a .ppf file, or from an archive holding one (the
// translator's .rar, or a .zip or .7z).
bool translation_read_patch(const char *path, std::vector<u8> *ppf, std::string *error);
// Downloads the translator's archive and extracts its patch.
bool translation_download(std::vector<u8> *ppf, std::string *error);
}

#endif // KF_PLATFORM_TRANSLATION_PPF_H
