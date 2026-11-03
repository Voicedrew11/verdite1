#include <kf/platform/translation_ppf.h>
#include <kf/platform/disc_layout.h>
#include <kf/platform/patch_archive.h>
#include <SDL3/SDL_iostream.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>

namespace kf {
namespace {
constexpr std::size_t raw_sector_bytes = 2352;
constexpr std::size_t sector_data_offset = 24;   // MODE2 form 1: sync, header, subheader
constexpr std::size_t sector_data_bytes = 2048;
constexpr std::size_t ppf_header_bytes = 0x3C;
constexpr std::size_t ppf_block_check_bytes = 1024;

// Through SDL, which also opens the content:// URI Android's file picker gives. To the stream's
// end, as a document provider's stream need not know its size.
bool read_file(const char *path, std::vector<u8> *bytes) {
    SDL_IOStream *file = SDL_IOFromFile(path, "rb");
    if (!file)
        return false;
    std::vector<u8> chunk(std::size_t{1} << 16);
    bytes->clear();
    bool ok = true;
    while (ok) {
        const std::size_t got = SDL_ReadIO(file, chunk.data(), chunk.size());
        if (got == 0) {
            ok = SDL_GetIOStatus(file) == SDL_IO_STATUS_EOF;
            break;
        }
        ok = bytes->size() + got <= patch_file_limit;
        if (ok)
            bytes->insert(bytes->end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(got));
    }
    return SDL_CloseIO(file) && ok;
}
}

bool translation_apply_ppf(AssetTable &assets, const std::vector<u8> &ppf, std::string *error) {
    if (assets_classify(assets) != DiscMatch::Japanese) {
        *error = "The English patch applies to the untouched Japanese game files.";
        return false;
    }
    if (ppf.size() < ppf_header_bytes || std::memcmp(ppf.data(), "PPF30", 5) != 0 || ppf[5] != 2) {
        *error = "That file is not a PPF 3.0 patch.";
        return false;
    }
    // Which file (and which of its sectors) each disc sector belongs to.
    std::vector<Asset *> files;
    std::vector<s32> owner;
    for (const auto &entry : slps00017_layout) {
        auto *asset = assets_find(assets, entry.path);
        if (!asset || asset->bytes.size() != entry.size) {
            *error = "The Japanese game files are incomplete.";
            return false;
        }
        const std::size_t sectors = std::max<std::size_t>(1, (entry.size + sector_data_bytes - 1) / sector_data_bytes);
        if (owner.size() < entry.sector + sectors)
            owner.resize(entry.sector + sectors, -1);
        for (std::size_t i = 0; i < sectors; ++i)
            owner[entry.sector + i] = static_cast<s32>(files.size());
        files.push_back(asset);
    }
    std::size_t position = ppf_header_bytes + (ppf[0x39] ? ppf_block_check_bytes : 0);
    const bool undo = ppf[0x3A] != 0;
    std::size_t end = ppf.size();
    static const char description[] = "@BEGIN_FILE_ID.DIZ";
    if (auto found = std::search(ppf.begin(), ppf.end(), description, description + sizeof description - 1); found != ppf.end())
        end = static_cast<std::size_t>(found - ppf.begin());
    std::size_t changed = 0;
    while (position < end) {
        if (position + 9 > end) {
            *error = "The patch is truncated.";
            return false;
        }
        std::uint64_t offset = 0;
        for (int i = 7; i >= 0; --i)
            offset = offset << 8 | ppf[position + i];
        const std::size_t length = ppf[position + 8];
        position += 9;
        if (position + length > end) {
            *error = "The patch is truncated.";
            return false;
        }
        for (std::size_t i = 0; i < length; ++i) {
            const std::uint64_t at = offset + i;
            const std::uint64_t sector = at / raw_sector_bytes;
            const std::size_t within = static_cast<std::size_t>(at % raw_sector_bytes);
            // Sector headers and error correction are not part of the files.
            if (within < sector_data_offset || within >= sector_data_offset + sector_data_bytes ||
                sector >= owner.size() || owner[sector] < 0)
                continue;
            const auto &entry = slps00017_layout[owner[sector]];
            const std::size_t file_offset = (sector - entry.sector) * sector_data_bytes + within - sector_data_offset;
            if (file_offset < entry.size) {
                files[owner[sector]]->bytes[file_offset] = ppf[position + i];
                ++changed;
            }
        }
        position += length * (undo ? 2 : 1);
    }
    if (!changed || assets_classify(assets) != DiscMatch::English) {
        *error = "The patch did not produce the English translation v1.0. Use John Osborne's "
                 "\"KF Jap to Eng v1.0\" patch.";
        return false;
    }
    return true;
}

bool translation_read_patch(const char *path, std::vector<u8> *ppf, std::string *error) {
    std::vector<u8> bytes;
    if (!read_file(path, &bytes)) {
        *error = "Cannot read the patch file.";
        return false;
    }
    // A picked content:// file's URI need not end in its name, so the bytes decide too.
    auto extension = std::filesystem::path(reinterpret_cast<const char8_t *>(path)).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    constexpr char ppf_magic[] = {'P', 'P', 'F'};
    if (extension == ".ppf" || (bytes.size() >= sizeof ppf_magic && std::memcmp(bytes.data(), ppf_magic, sizeof ppf_magic) == 0)) {
        *ppf = std::move(bytes);
        return true;
    }
    auto unpacked = patch_from_archive(bytes, error);
    if (!unpacked)
        return false;
    *ppf = std::move(*unpacked);
    return true;
}

bool translation_download(std::vector<u8> *ppf, std::string *error) {
    const auto archive = patch_archive_download(english_patch_url, error);
    if (!archive)
        return false;
    Sha256 hash{};
    sha256_init(&hash);
    sha256_update(&hash, *archive);
    std::array<char, sha256_hex_capacity> digest;
    sha256_finish(&hash, digest);
    if (std::strcmp(digest.data(), english_patch_archive_sha256) != 0) {
        *error = "The downloaded file is not the translator's v1.0 release.";
        return false;
    }
    auto unpacked = patch_from_archive(*archive, error);
    if (!unpacked)
        return false;
    *ppf = std::move(*unpacked);
    return true;
}
}
