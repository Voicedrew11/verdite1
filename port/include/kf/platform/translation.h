#ifndef KF_PLATFORM_TRANSLATION_H
#define KF_PLATFORM_TRANSLATION_H

#include <kf/platform/assets.h>

#include <span>

namespace kf {
// On failure the caller must discard the entire table, not publish partial data.
bool translation_apply(AssetTable &assets, std::span<const u8> patch);
bool translation_available();
const char *assets_prepare_language(AssetTable &assets, Language language);
}

#endif // KF_PLATFORM_TRANSLATION_H
