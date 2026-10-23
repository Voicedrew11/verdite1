#include <kf/platform/assets.h>
#include <kf/platform/translation.h>

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <new>
#include <string_view>
#include <utility>

namespace kf {
namespace {
constexpr unsigned maximum_tim_dimension = 4096;
constexpr unsigned sha256_round_count = 64;
constexpr unsigned sha256_input_words = 16;
constexpr unsigned sha256_length_offset = 56;
constexpr u8 sha256_padding_marker = 0x80;
constexpr u32 iso_payload_bytes = 2048;
constexpr u32 cd_raw_sector_bytes = 2352;
constexpr unsigned cd_mode_offset = 15;
constexpr unsigned cd_mode2 = 2;
constexpr unsigned cd_subheader_offset = 16;
constexpr unsigned cd_subheader_copy_offset = 20;
constexpr unsigned cd_subheader_bytes = 4;
constexpr unsigned cd_submode_offset = 18;
constexpr unsigned cd_form2_flag = 0x20;
constexpr u32 cd_mode2_payload_offset = 24;
constexpr unsigned iso_primary_volume_sector = 16;
constexpr unsigned iso_minimum_record_bytes = 34;
constexpr unsigned iso_extended_attributes_offset = 1;
constexpr unsigned iso_extent_le_offset = 2;
constexpr unsigned iso_extent_be_offset = 6;
constexpr unsigned iso_length_le_offset = 10;
constexpr unsigned iso_length_be_offset = 14;
constexpr unsigned iso_flags_offset = 25;
constexpr unsigned iso_file_unit_offset = 26;
constexpr unsigned iso_interleave_gap_offset = 27;
constexpr unsigned iso_name_length_offset = 32;
constexpr unsigned iso_name_offset = 33;
constexpr unsigned iso_directory_flag = 2;
constexpr unsigned iso_multi_extent_flag = 0x80;
constexpr unsigned iso_volume_id_offset = 40;
constexpr unsigned iso_volume_id_bytes = 32;
constexpr unsigned iso_volume_size_le_offset = 80;
constexpr unsigned iso_volume_size_be_offset = 84;
constexpr unsigned iso_block_size_offset = 128;
constexpr unsigned iso_root_record_offset = 156;
constexpr std::size_t maximum_extent_bytes = 16 * 1024 * 1024;
constexpr int ascii_first_printable = 32;
constexpr int ascii_delete = 127;
constexpr double import_metadata_progress = 0.1;
constexpr double import_file_progress = 0.9;
}

bool image_decode_tim(Image *image, const u8 *data, std::size_t size, std::size_t offset,
                      u32 palette) {
    KfTimInfo info{};
    if (kf_tim_info({data, size}, offset, info) != KF_CODEC_OK)
        return false;
    if (info.width > maximum_tim_dimension || info.height > maximum_tim_dimension)
        return false;
    Image decoded{info.width, info.height, {}};
    try {
        decoded.rgba.resize(static_cast<std::size_t>(info.width) * info.height * 4);
    } catch (const std::bad_alloc &) {
        return false;
    }
    if (kf_tim_rgba({data, size}, offset, palette, decoded.rgba) !=
        KF_CODEC_OK) {
        return false;
    }
    *image = std::move(decoded);
    return true;
}
bool asset_path(std::span<char> output, const char *input) {
    std::size_t size = 0;
    constexpr std::string_view prefix = "CDROM:";
    std::size_t matched = 0;
    while (matched < prefix.size() && input[matched]) {
        char c = input[matched];
        if (c >= 'a' && c <= 'z')
            c -= 'a' - 'A';
        if (c != prefix[matched])
            break;
        ++matched;
    }
    if (matched == prefix.size())
        input += prefix.size();
    while (*input == '/' || *input == '\\')
        ++input;
    while (*input && *input != ';') {
        if (size + 1 >= output.size())
            return false;
        char c = *input++;
        if (c == '\\')
            c = '/';
        if (c >= 'a' && c <= 'z')
            c -= 'a' - 'A';
        if (static_cast<unsigned char>(c) < ascii_first_printable || static_cast<unsigned char>(c) >= ascii_delete || c == ':')
            return false;
        output[size++] = c;
    }
    if (!size || output.empty())
        return false;
    output[size] = 0;
    const char *segment = output.data();
    for (const char *at = output.data();; ++at) {
        if (*at && *at != '/')
            continue;
        const auto length = at - segment;
        if (!length || (length == 1 && *segment == '.') ||
            (length == 2 && segment[0] == '.' && segment[1] == '.'))
            return false;
        if (!*at)
            break;
        segment = at + 1;
    }
    return true;
}
bool assets_append(AssetTable &table, const char *path, ByteBuffer &&bytes) {
    std::array<char, asset_path_capacity> normalized{};
    if (table.size() == table.max_size() ||
        !asset_path(normalized, path) || assets_find(table, normalized.data()))
        return false;
    try {
        table.emplace_back(normalized, std::move(bytes));
    } catch (const std::bad_alloc &) {
        return false;
    }
    return true;
}
Asset *assets_find(AssetTable &table, const char *path) {
    std::array<char, asset_path_capacity> normalized{};
    if (!asset_path(normalized, path))
        return nullptr;
    for (auto &asset : table)
        if (std::strcmp(asset.path.data(), normalized.data()) == 0)
            return &asset;
    return nullptr;
}

static constexpr std::array<u32, sha256_round_count> sha_constants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
void sha256_init(Sha256 *hash) {
    *hash = {{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
              0x5be0cd19},
             {},
             0,
             0};
}
static void sha256_block(Sha256 *hash, std::span<const u8, sha256_block_bytes> data) {
    std::array<u32, sha256_round_count> words;
    for (std::size_t i = 0; i < sha256_input_words; ++i)
        words[i] = (u32(data[i * 4]) << 24) | (u32(data[i * 4 + 1]) << 16) |
                   (u32(data[i * 4 + 2]) << 8) | data[i * 4 + 3];
    for (std::size_t i = sha256_input_words; i < sha256_round_count; ++i) {
        const auto a = words[i - 15], b = words[i - 2];
        words[i] = words[i - 16] + (std::rotr(a, 7) ^ std::rotr(a, 18) ^ (a >> 3)) + words[i - 7] +
                   (std::rotr(b, 17) ^ std::rotr(b, 19) ^ (b >> 10));
    }
    u32 a = hash->state[0], b = hash->state[1], c = hash->state[2], d = hash->state[3];
    u32 e = hash->state[4], f = hash->state[5], g = hash->state[6], h = hash->state[7];
    for (std::size_t i = 0; i < sha256_round_count; ++i) {
        const auto t1 = h + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) +
                        ((e & f) ^ (~e & g)) + sha_constants[i] + words[i];
        const auto t2 =
            (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    const std::array<u32, 8> result = {a, b, c, d, e, f, g, h};
    for (std::size_t i = 0; i < result.size(); ++i)
        hash->state[i] += result[i];
}
void sha256_update(Sha256 *hash, std::span<const u8> bytes) {
    hash->length += bytes.size();
    while (!bytes.empty()) {
        const auto count = std::min(hash->pending.size() - hash->used, bytes.size());
        std::copy_n(bytes.begin(), count, hash->pending.begin() + hash->used);
        hash->used += count;
        bytes = bytes.subspan(count);
        if (hash->used == hash->pending.size()) {
            sha256_block(hash, hash->pending);
            hash->used = 0;
        }
    }
}
void sha256_finish(const Sha256 *input, std::span<char, sha256_hex_capacity> output) {
    auto hash = *input;
    const auto bits = hash.length * 8;
    const u8 marker = sha256_padding_marker, zero = 0;
    sha256_update(&hash, {&marker, 1});
    while (hash.used != sha256_length_offset)
        sha256_update(&hash, {&zero, 1});
    std::array<u8, 8> length;
    for (std::size_t i = 0; i < length.size(); ++i)
        length[i] = static_cast<u8>(bits >> ((length.size() - 1 - i) * 8));
    sha256_update(&hash, length);
    for (std::size_t i = 0; i < hash.state.size(); ++i)
        std::snprintf(output.data() + i * 8, 9, "%08x", hash.state[i]);
}

static u32 le32(const u8 *p) {
    return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
}
static u32 be32(const u8 *p) {
    return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]);
}
const char *assets_language_hash(Language language) {
    return language == Language::English ? english_v1_files_sha256 : retail_files_sha256;
}
// Digests of every file, and of every file except GAME.EXE (see english_resources_sha256).
static bool assets_digests(AssetTable &table, std::array<char, sha256_hex_capacity> &files,
                           std::array<char, sha256_hex_capacity> &resources) {
    if (table.size() != retail_resource_file_count)
        return false;
    std::sort(table.begin(), table.end(),
              [](const Asset &a, const Asset &b) { return std::strcmp(a.path.data(), b.path.data()) < 0; });
    Sha256 hash{}, without_exe{};
    sha256_init(&hash);
    sha256_init(&without_exe);
    std::size_t total = 0;
    for (const auto &asset : table) {
        const auto path_size = std::strlen(asset.path.data());
        if (asset.bytes.size() > disc_import_limit - total)
            return false;
        total += asset.bytes.size();
        std::array<u8, 8> sizes;
        for (std::size_t byte = 0; byte < 4; ++byte) {
            sizes[byte] = static_cast<u8>(path_size >> (byte * 8));
            sizes[byte + 4] = static_cast<u8>(asset.bytes.size() >> (byte * 8));
        }
        const std::span<const u8> path{reinterpret_cast<const u8 *>(asset.path.data()), path_size};
        sha256_update(&hash, sizes);
        sha256_update(&hash, path);
        sha256_update(&hash, asset.bytes);
        if (std::strcmp(asset.path.data(), "GAME.EXE") != 0) {
            sha256_update(&without_exe, sizes);
            sha256_update(&without_exe, path);
            sha256_update(&without_exe, asset.bytes);
        }
    }
    sha256_finish(&hash, files);
    sha256_finish(&without_exe, resources);
    return true;
}
bool assets_match_language(AssetTable &table, Language language) {
    std::array<char, sha256_hex_capacity> files, resources;
    if (!assets_digests(table, files, resources))
        return false;
    if (std::strcmp(files.data(), assets_language_hash(language)) == 0)
        return true;
    // English v1.0 with the save-bug fix differs only in GAME.EXE, which never runs.
    return language == Language::English && std::strcmp(resources.data(), english_resources_sha256) == 0;
}
DiscMatch assets_classify(AssetTable &table) {
    std::array<char, sha256_hex_capacity> files, resources;
    if (!assets_digests(table, files, resources))
        return DiscMatch::Foreign;
    if (std::strcmp(files.data(), retail_files_sha256) == 0)
        return DiscMatch::Japanese;
    if (std::strcmp(files.data(), english_v1_files_sha256) == 0 ||
        std::strcmp(resources.data(), english_resources_sha256) == 0)
        return DiscMatch::English;
    std::printf("Disc files differ from the Japanese release and the English translation v1.0; using them as they are.\n"
                "Files digest: %s\n", files.data());
    return DiscMatch::Modified;
}
static void read_extent(DiscImporter *importer, const DiscExtent *file) {
    const auto sectors = (std::uint64_t(file->length) + iso_payload_bytes - 1) / iso_payload_bytes;
    const auto offset = std::uint64_t(file->sector) * importer->sector_size;
    const auto size = sectors * importer->sector_size;
    if (file->length > maximum_extent_bytes || offset > importer->disc_size ||
        size > importer->disc_size - offset ||
        (importer->volume_sectors && (file->sector > importer->volume_sectors ||
         sectors > importer->volume_sectors - file->sector))) {
        disc_import_fail(importer, "Invalid or oversized disc extent.");
        return;
    }
    importer->current = *file;
    importer->request = {offset, static_cast<u32>(size)};
}
static void next_file(DiscImporter *importer) {
    if (importer->file_index == importer->file_count) {
        // PC extension: the Windows flow accepts every release with the retail
        // file set (such as the English translation) and sorts them afterwards.
        if (importer->any_release) {
            if (assets_classify(importer->assets) == DiscMatch::Foreign) {
                disc_import_fail(importer, "This disc image is not King's Field (Japan, SLPS-00017).");
                return;
            }
        } else if (const char *error = assets_prepare_language(importer->assets, importer->language)) {
            disc_import_fail(importer, error);            return;
        }
        importer->state = ImportState::complete;
        importer->request = {};
        std::snprintf(importer->message.data(), importer->message.size(), "Imported %zu resource files.",
                      importer->assets.size());
        return;
    }
    importer->state = ImportState::file;
    read_extent(importer, &importer->files[importer->file_index++]);
}
static void next_directory(DiscImporter *importer) {
    if (!importer->directory_count) {
        next_file(importer);
        return;
    }
    importer->state = ImportState::directory;
    const auto directory = importer->directories[--importer->directory_count];
    read_extent(importer, &directory);
}
void disc_import_fail(DiscImporter *importer, const char *message) {
    std::snprintf(importer->message.data(), importer->message.size(), "%s", message);
    importer->state = ImportState::failed;
    importer->request = {};
    importer->assets.clear();
}
void disc_import_start(DiscImporter *importer, std::uint64_t disc_size, Language language) {
    *importer = {};
    importer->language = language;
    if (disc_size < (iso_primary_volume_sector + 1) * iso_payload_bytes || disc_size > disc_import_limit) {
        disc_import_fail(importer, "Expected an SLPS-00017 ISO or BIN image (up to 128 MiB).");
        return;
    }
    importer->disc_size = disc_size;
    importer->state = ImportState::layout;
    importer->request = {0, cd_raw_sector_bytes};
    std::snprintf(importer->message.data(), importer->message.size(),
                  "Verifying %s SLPS-00017 disc...", language_name(language));
}
bool disc_import_waiting(const DiscImporter *importer) {
    return importer->state >= ImportState::layout && importer->state <= ImportState::file;
}
static bool unpack_payload(DiscImporter *importer, std::span<const u8> raw,
                           ByteBuffer &output) {
    const auto length = importer->current.length;
    try {
        output.resize(length);
    } catch (const std::bad_alloc &) {
        return false;
    }
    for (std::size_t at = 0, sector = 0; at < length; at += iso_payload_bytes, sector += importer->sector_size) {
        auto count = length - at;
        if (count > iso_payload_bytes)
            count = iso_payload_bytes;
        const auto header = importer->sector_size == cd_raw_sector_bytes ? cd_mode2_payload_offset : 0u;
        if (sector + header + count > raw.size())
            return false;
        if (header && (raw[sector + cd_mode_offset] != cd_mode2 || (raw[sector + cd_submode_offset] & cd_form2_flag) ||
                       std::memcmp(raw.data() + sector + cd_subheader_offset, raw.data() + sector + cd_subheader_copy_offset, cd_subheader_bytes) != 0))
            return false;
        std::memcpy(output.data() + at, raw.data() + sector + header, count);
    }
    return true;
}
static bool parse_directory(DiscImporter *importer, std::span<const u8> bytes) {
    for (std::size_t at = 0; at < bytes.size();) {
        const auto length = bytes.data()[at];
        if (!length) {
            at = (at / iso_payload_bytes + 1) * iso_payload_bytes;
            continue;
        }
        if (length < iso_minimum_record_bytes || length > bytes.size() - at || at % iso_payload_bytes + length > iso_payload_bytes)
            return false;
        const auto *row = bytes.data() + at;
        const auto name_length = row[iso_name_length_offset];
        if (iso_name_offset + name_length > length)
            return false;
        if (name_length == 1 && row[iso_name_offset] <= 1) {
            at += length;
            continue;
        }
        // Multi-extent/interleaved files are not present on the supported disc.
        if ((row[iso_flags_offset] & iso_multi_extent_flag) || row[iso_extended_attributes_offset] || row[iso_file_unit_offset] || row[iso_interleave_gap_offset] ||
            le32(row + iso_extent_le_offset) != be32(row + iso_extent_be_offset) || le32(row + iso_length_le_offset) != be32(row + iso_length_be_offset))
            return false;
        std::array<char, asset_path_capacity> name{};
        if (!name_length || name_length >= name.size())
            return false;
        std::memcpy(name.data(), row + iso_name_offset, name_length);
        for (std::size_t i = 0; i < name_length; ++i)
            if (name[i] < ascii_first_printable || name[i] >= ascii_delete || name[i] == '/' || name[i] == '\\')
                return false;
        if (const char *version = std::strchr(name.data(), ';');
            version && ((row[iso_flags_offset] & iso_directory_flag) || std::strcmp(version, ";1") != 0))
            return false;
        DiscExtent file{};
        const int written =
            std::snprintf(file.path.data(), file.path.size(), "%s%s%s", importer->current.path.data(),
                          importer->current.path[0] ? "/" : "", name.data());
        if (written < 0 || std::size_t(written) >= file.path.size())
            return false;
        file.sector = le32(row + iso_extent_le_offset);
        file.length = le32(row + iso_length_le_offset);
        std::array<char, asset_path_capacity> normalized{};
        if (!asset_path(normalized, file.path.data()))
            return false;
        file.path = normalized;
        if (row[iso_flags_offset] & iso_directory_flag) {
            for (std::size_t i = 0; i < importer->visited_count; ++i)
                if (importer->visited_directories[i] == file.sector)
                    return false;
            if (importer->visited_count == importer->visited_directories.size() || importer->directory_count == importer->directories.size())
                return false;
            importer->visited_directories[importer->visited_count++] = file.sector;
            importer->directories[importer->directory_count++] = file;
        } else {
            if (importer->file_count == importer->files.size() || file.length > disc_import_limit - importer->file_bytes)
                return false;
            importer->file_bytes += file.length;
            importer->files[importer->file_count++] = file;
        }
        at += length;
    }
    return true;
}
void disc_import_supply(DiscImporter *importer, std::span<const u8> bytes) {
    if (!disc_import_waiting(importer))
        return;
    if (bytes.size() != importer->request.length) {
        disc_import_fail(importer, "Disc read was incomplete.");
        return;
    }
    if (importer->state == ImportState::layout) {
        constexpr std::array<u8, 12> sync = {0,255,255,255,255,255,255,255,255,255,255,0};
        importer->sector_size = std::memcmp(bytes.data(), sync.data(), sync.size()) == 0 ? cd_raw_sector_bytes : iso_payload_bytes;
        if (importer->disc_size % importer->sector_size != 0) {
            disc_import_fail(importer, "Image has an incomplete data sector.");
            return;
        }
        importer->state = ImportState::volume;
        const DiscExtent volume{{}, iso_primary_volume_sector, iso_payload_bytes};
        read_extent(importer, &volume);
        return;
    }
    ByteBuffer payload{};
    if (!unpack_payload(importer, bytes, payload)) {
        disc_import_fail(importer, "Invalid data sector or allocation failure.");
        return;
    }
    if (importer->state == ImportState::volume) {
        if (payload.size() != iso_payload_bytes || std::memcmp(payload.data(), "\1CD001\1", 7) != 0 ||
            std::memcmp(payload.data() + iso_volume_id_offset, "SLPS-00017                      ", iso_volume_id_bytes) != 0 ||
            payload.data()[iso_block_size_offset] != 0 || payload.data()[iso_block_size_offset + 1] != 8 ||
            payload.data()[iso_block_size_offset + 2] != 8 || payload.data()[iso_block_size_offset + 3] != 0 ||
            le32(payload.data() + iso_volume_size_le_offset) != be32(payload.data() + iso_volume_size_be_offset) ||
            le32(payload.data() + iso_volume_size_le_offset) > importer->disc_size / importer->sector_size) {
            disc_import_fail(importer, "Unsupported ISO9660 volume; Japanese SLPS-00017 is required.");
        } else {
            const auto *root = payload.data() + iso_root_record_offset;
            if (root[0] < iso_minimum_record_bytes || root[iso_extended_attributes_offset] || !(root[iso_flags_offset] & iso_directory_flag) || root[iso_file_unit_offset] || root[iso_interleave_gap_offset] ||
                le32(root + iso_extent_le_offset) != be32(root + iso_extent_be_offset) || le32(root + iso_length_le_offset) != be32(root + iso_length_be_offset)) {
                disc_import_fail(importer, "Invalid ISO9660 root directory.");
            } else {
                importer->volume_sectors = le32(payload.data() + iso_volume_size_le_offset);
                importer->directories[importer->directory_count++] = {
                    {}, le32(root + iso_extent_le_offset), le32(root + iso_length_le_offset)};
                importer->visited_directories[importer->visited_count++] = le32(root + iso_extent_le_offset);
                next_directory(importer);
            }
        }
    } else if (importer->state == ImportState::directory) {
        if (!parse_directory(importer, payload))
            disc_import_fail(importer, "Invalid ISO9660 directory.");
        else
            next_directory(importer);
    } else {
        if (!assets_append(importer->assets, importer->current.path.data(), std::move(payload)))
            disc_import_fail(importer, "Duplicate resource name or allocation failure.");
        else
            next_file(importer);
    }
}
double disc_import_progress(const DiscImporter *importer) {
    if (!importer->disc_size)
        return 0;
    if (importer->state == ImportState::complete)
        return 1;
    return import_metadata_progress + (importer->file_count
                      ? import_file_progress * double(importer->assets.size()) / double(importer->file_count)
                      : 0);
}
} // namespace kf
