#include "english_patch.inc"
#include <kf/lib/byte_reader.h>
#include <kf/platform/translation.h>

#include <cstring>

namespace kf {
bool translation_apply(AssetTable &assets, std::span<const u8> patch) try {
    constexpr std::array<u8, 8> magic = {'K', 'F', 'E', 'N', 1, 0, 0, 0};
    if (patch.size() < magic.size() || std::memcmp(patch.data(), magic.data(), magic.size()))
        return false;
    codec::Reader reader{patch.subspan(magic.size())};
    const u32 files = reader.u32_le();
    if (!files || files > assets.size())
        return false;
    std::array<char, asset_path_capacity> previous{};
    for (u32 file = 0; file < files; ++file) {
        const u32 name_length = reader.u32_le();
        const u32 file_size = reader.u32_le();
        const u32 spans = reader.u32_le();
        if (!name_length || name_length >= asset_path_capacity || !spans || spans > file_size)
            return false;
        std::array<char, asset_path_capacity> name{};
        const auto name_bytes = reader.take(name_length);
        std::memcpy(name.data(), name_bytes.data(), name_bytes.size());
        std::array<char, asset_path_capacity> normalized;
        if (std::memchr(name.data(), 0, name_length) || !asset_path(normalized, name.data()) ||
            std::strcmp(name.data(), normalized.data()) || std::strcmp(previous.data(), name.data()) >= 0)
            return false;
        previous = name;
        auto *asset = assets_find(assets, name.data());
        if (!asset || asset->bytes.size() != file_size)
            return false;
        std::size_t end = 0;
        for (u32 span = 0; span < spans; ++span) {
            const u32 offset = reader.u32_le();
            const u32 size = reader.u32_le();
            if (!size || offset < end || offset > file_size || size > file_size - offset)
                return false;
            const auto replacement = reader.take(size);
            std::memcpy(asset->bytes.data() + offset, replacement.data(), replacement.size());
            end = std::size_t(offset) + size;
        }
    }
    return reader.remaining() == 0;
} catch (const codec::Error &) {
    return false;
}

bool translation_available() { return !english_patch_data.empty(); }

const char *assets_prepare_language(AssetTable &assets, Language language) {
    if (assets_match_language(assets, language))
        return nullptr;
    if (language != Language::English || !assets_match_language(assets, Language::Japanese))
        return "Resources do not match the supported Japanese SLPS-00017 disc or selected language.";
    if (english_patch_data.empty())
        return "English translation data is not included in this build. Japanese remains available.";
    if (!translation_apply(assets, english_patch_data) ||
        !assets_match_language(assets, Language::English))
        return "English resource generation failed verification. No cache was published.";
    return nullptr;
}
}
