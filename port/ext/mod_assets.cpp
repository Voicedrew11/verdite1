#include "mod_assets.h"

#include <kf/audio/codec.h>
#include <kf/lib/codec.h>
#include <kf/lib/tmd.h>
#include <kf/platform/files.h>
#include <kf/renderer/constants.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace kf::ext {
namespace {

using Bytes = std::span<const std::uint8_t>;
using Refusal = std::optional<std::string>;

Refusal check_tim(Bytes bytes);
Refusal check_seq(Bytes bytes);
Refusal check_tmd(Bytes bytes);
// The kinds' checks, given the disc path; only a picture inside a file needs it.
Refusal tim_row(std::string_view, Bytes bytes) { return check_tim(bytes); }
Refusal seq_row(std::string_view, Bytes bytes) { return check_seq(bytes); }
Refusal tmd_row(std::string_view, Bytes bytes) { return check_tmd(bytes); }
Refusal check_picture(std::string_view path, Bytes bytes);

// Pictures a mod may replace inside a file of several TIMs one after another, named "<file>#<n>" with
// n counted from 0: the pages of the opening and ending scrolls (B0/MIX3., B0/MIX9.) and the page of
// notification banners in the common pictures (COM/MIX.TIM). Only these: the files' other pictures
// are textures the scenes' models are mapped onto by position.
struct PicturePlace {
    std::string_view path, file;
    std::size_t index;
};
constexpr std::array<PicturePlace, 5> picture_places{{
    {"KF/B0/MIX3.#6", "KF/B0/MIX3.", 6},
    {"KF/B0/MIX3.#7", "KF/B0/MIX3.", 7},
    {"KF/B0/MIX9.#7", "KF/B0/MIX9.", 7},
    {"KF/B0/MIX9.#8", "KF/B0/MIX9.", 8},
    {"KF/COM/MIX.TIM#5", "KF/COM/MIX.TIM", 5},
}};


// The kinds a mod may replace, by suffix: a file takes the first row its disc path ends with.
// No MIM row: a MIM with fewer animation clips than the game asks for passes any byte check (docs/MODDING.md).
const std::array<verdite::ModAssetKind, 37> asset_kinds{{
    {".TIM", tim_row},
    {".SEQ", seq_row},
    {".TMD", tmd_row},
    // Pictures the disc names without .TIM: the spells' pages, the memory card's and quit's
    // messages (KF/TIM), and the boxed system screens at the root (disc errors, no card, pause).
    {"KF/TIM/M001.", tim_row}, {"KF/TIM/M002.", tim_row}, {"KF/TIM/M003.", tim_row}, {"KF/TIM/M004.", tim_row},
    {"KF/TIM/M005.", tim_row}, {"KF/TIM/M006.", tim_row}, {"KF/TIM/M007.", tim_row}, {"KF/TIM/M008.", tim_row},
    {"KF/TIM/M009.", tim_row}, {"KF/TIM/M101.", tim_row}, {"KF/TIM/M102.", tim_row}, {"KF/TIM/M103.", tim_row},
    {"KF/TIM/M104.", tim_row}, {"KF/TIM/M105.", tim_row}, {"KF/TIM/M106.", tim_row}, {"KF/TIM/M107.", tim_row},
    {"KF/TIM/M108.", tim_row}, {"KF/TIM/M109.", tim_row}, {"KF/TIM/M110.", tim_row}, {"KF/TIM/M111.", tim_row},
    {"KF/TIM/M112.", tim_row}, {"KF/TIM/M113.", tim_row}, {"KF/TIM/M114.", tim_row}, {"KF/TIM/M115.", tim_row},
    {"KF/TIM/M999.", tim_row}, {"E0.", tim_row}, {"E1.", tim_row}, {"E2.", tim_row},
    {"E3.", tim_row},
    {picture_places[0].path, check_picture}, {picture_places[1].path, check_picture},
    {picture_places[2].path, check_picture}, {picture_places[3].path, check_picture},
    {picture_places[4].path, check_picture},
}};

// Nothing if decode accepts the bytes, else why the game would not load them. The decoders throw.
template <typename Decode>
Refusal refusal_of(std::string_view kind, Decode decode) {
    std::string reason;
    try {
        decode();
        return std::nullopt;
    } catch (const kf::codec::Error &error) {
        reason = error.message;
    } catch (const std::runtime_error &error) {
        reason = error.what();
    } catch (const std::bad_alloc &) {
        reason = "out of memory";
    } catch (...) {
        reason = "the decoder failed";
    }
    return "is not a " + std::string(kind) + " the game can load: " + reason;
}

// The parse texture_store_upload_tim makes, into a scratch word image; the live store is not touched.
Refusal check_tim(Bytes bytes) {
    return refusal_of("TIM", [&] {
        std::vector<u16> scratch(static_cast<std::size_t>(texture_store_width) * texture_store_height);
        kf_tim_compose(bytes, scratch);
    });
}

Refusal check_seq(Bytes bytes) {
    return refusal_of("SEQ", [&] {
        std::vector<KfMusicEvent> events;
        KfMusicInfo info{};
        kf_music_decode(bytes, events, info);
    });
}

// ---- TMD: what a draw of the model reads (tmd.cpp, render_enqueuers.cpp) ----
//
// Each test below is one the game makes on a TMD before it draws, and each failure there is a
// host_fail, which stops the game. tmd_resource_view, the game's own header check, is not called
// here: it stops the game rather than returning, and this check runs before a file is in use.
// The TMD is assumed to start on a 4-byte boundary, as the game's buffers do.

struct TmdObject {
    u32 vertex_offset, vertex_count, normal_offset, normal_count, primitive_offset, primitive_count;
};

u32 word_at(Bytes bytes, std::size_t offset) { return tmd_read_word(bytes.data() + offset); }

TmdObject object_at(Bytes tmd, u32 index) {
    const std::size_t at = KF_TMD_HEADER_BYTES + sizeof(KfTmdObject) * index;
    return {word_at(tmd, at + offsetof(KfTmdObject, vertex_offset)),
        word_at(tmd, at + offsetof(KfTmdObject, vertex_count)),
        word_at(tmd, at + offsetof(KfTmdObject, normal_offset)),
        word_at(tmd, at + offsetof(KfTmdObject, normal_count)),
        word_at(tmd, at + offsetof(KfTmdObject, primitive_offset)),
        word_at(tmd, at + offsetof(KfTmdObject, primitive_count))};
}

// tmd_select_object_vertices, tmd_normal_bytes and tmd_primitive_stream: each array lies in the file.
Refusal array_refusal(const TmdObject &object, std::size_t payload) {
    if (object.vertex_offset > payload || object.vertex_count > (payload - object.vertex_offset) / sizeof(SVECTOR))
        return "its vertices run past the end of the file";
    if (object.vertex_offset % alignof(SVECTOR))
        return "its vertices are not aligned";
    if (object.vertex_count > KF_PROJECTED_VERTEX_CAPACITY)
        return "an object has more vertices than the game projects";
    if (object.normal_count && (object.normal_offset > payload || object.normal_count > (payload - object.normal_offset) / 8))
        return "its normals run past the end of the file";
    if (object.primitive_count && (object.primitive_offset > payload ||
            object.primitive_count > (payload - object.primitive_offset) / KF_TMD_PACKET_HEADER_BYTES))
        return "its primitives run past the end of the file";
    return std::nullopt;
}

struct FaceLayout {
    std::size_t corners;
    bool textured;
    bool gouraud;
};

// The faces the game decodes (tmd_decode_face's layouts); other packets are not read.
std::optional<FaceLayout> face_layout(KfTmdMode kind) {
    switch (kind) {
    case KF_TMD_MODE_F3:  return FaceLayout{3, false, false};
    case KF_TMD_MODE_F4:  return FaceLayout{4, false, false};
    case KF_TMD_MODE_G3:  return FaceLayout{3, false, true};
    case KF_TMD_MODE_G4:  return FaceLayout{4, false, true};
    case KF_TMD_MODE_FT3: return FaceLayout{3, true, false};
    case KF_TMD_MODE_FT4: return FaceLayout{4, true, false};
    case KF_TMD_MODE_GT3: return FaceLayout{3, true, true};
    case KF_TMD_MODE_GT4: return FaceLayout{4, true, true};
    default: return std::nullopt;
    }
}

u16 half_at(const u8 *bytes) { return static_cast<u16>(bytes[0] | (bytes[1] << 8)); }

// tmd_decode_face's bounds for one face, and the indices it reads (vertices and normals).
Refusal face_refusal(const FaceLayout &layout, Bytes body, const TmdObject &object) {
    const std::size_t corners = layout.corners;
    const std::size_t index_offset = layout.textured ? corners * 4 : 4;
    const std::size_t index_bytes = layout.gouraud ? corners * 4 : (corners + 1) * 2;
    if (body.size() < index_offset + index_bytes)
        return "a face is shorter than its layout";
    const u8 *indices = body.data() + index_offset;
    std::array<u16, 4> vertices{};
    std::array<u16, 4> normals{};
    if (!layout.gouraud)
        normals[0] = half_at(indices);
    for (std::size_t i = 0; i < corners; ++i) {
        if (layout.gouraud) {
            normals[i] = half_at(indices + i * 4);
            vertices[i] = half_at(indices + i * 4 + 2);
        } else {
            vertices[i] = half_at(indices + 2 + i * 2);
        }
    }
    for (std::size_t i = 0; i < corners; ++i) {
        if (vertices[i] >= object.vertex_count || vertices[i] >= KF_PROJECTED_VERTEX_CAPACITY)
            return "a face names a vertex its object does not have";
        if (normals[i] >= object.normal_count)
            return "a face names a normal its object does not have";
    }
    return std::nullopt;
}

// Walks one object's primitive packets as tmd_next_packet reads them.
Refusal primitives_refusal(Bytes payload, const TmdObject &object) {
    if (!object.primitive_count)
        return std::nullopt;
    Bytes rest = payload.subspan(object.primitive_offset);
    for (u32 n = 0; n < object.primitive_count; ++n) {
        if (rest.size() < KF_TMD_PACKET_HEADER_BYTES)
            return "a primitive header runs past the end of the file";
        const std::size_t body_size = static_cast<std::size_t>(rest[KF_TMD_ILEN_BYTE]) * KF_TMD_WORD_BYTES;
        if (body_size > rest.size() - KF_TMD_PACKET_HEADER_BYTES)
            return "a primitive body runs past the end of the file";
        const KfTmdMode kind = tmd_packet_kind(word_at(rest, 0));
        if (const auto layout = face_layout(kind))
            if (auto reason = face_refusal(*layout, rest.subspan(KF_TMD_PACKET_HEADER_BYTES, body_size), object))
                return reason;
        rest = rest.subspan(KF_TMD_PACKET_HEADER_BYTES + body_size);
    }
    return std::nullopt;
}

// A TMD the game can draw: a header, at least one object, and every array and face in range.
Refusal tmd_refusal(Bytes tmd) {
    if (tmd.size() < KF_TMD_HEADER_BYTES)
        return "is shorter than a TMD header";
    const u32 object_count = word_at(tmd, offsetof(KfTmdHeader, object_count));
    const std::size_t payload_size = tmd.size() - KF_TMD_HEADER_BYTES;
    if (object_count == 0)
        return "has no object to draw";
    if (object_count > payload_size / sizeof(KfTmdObject))
        return "its object table runs past the end of the file";
    const Bytes payload = tmd.subspan(KF_TMD_HEADER_BYTES);
    for (u32 index = 0; index < object_count; ++index) {
        const TmdObject object = object_at(tmd, index);
        if (auto reason = array_refusal(object, payload_size))
            return reason;
        if (auto reason = primitives_refusal(payload, object))
            return reason;
    }
    return std::nullopt;
}

Refusal check_tmd(Bytes bytes) {
    if (auto reason = tmd_refusal(bytes))
        return "is not a TMD the game can draw: " + *reason;
    return std::nullopt;
}

// ---- pictures inside a file of several ----

constexpr std::uint32_t tim_magic = 0x10;
constexpr std::uint32_t tim_depth_mask = 7;
constexpr std::uint32_t tim_has_palette = 8;
constexpr std::size_t tim_header_bytes = 8;
constexpr std::size_t tim_block_header_bytes = 12;

// Where a block's rectangle sits: x, y, width, height, after its length word.
struct TimBlock {
    std::size_t at = 0;   // its length word, from the start of the file
    u16 x = 0, y = 0, width = 0, height = 0;
};

// One TIM of a file: its bytes and its two blocks.
struct TimPicture {
    std::size_t at = 0, size = 0;
    std::uint32_t depth = 0;
    std::optional<TimBlock> palette;
    TimBlock image;
};

u16 u16_at(Bytes bytes, std::size_t at) {
    return static_cast<u16>(bytes[at] | (bytes[at + 1] << 8));
}

std::uint32_t u32_at(Bytes bytes, std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at]) | (static_cast<std::uint32_t>(bytes[at + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[at + 2]) << 16) | (static_cast<std::uint32_t>(bytes[at + 3]) << 24);
}

// A block's rectangle, and the end of the block; nothing if it runs past the bytes.
std::optional<std::pair<TimBlock, std::size_t>> tim_block(Bytes bytes, std::size_t at) {
    if (bytes.size() - at < tim_block_header_bytes)
        return std::nullopt;
    const std::uint32_t length = u32_at(bytes, at);
    if (length < tim_block_header_bytes || length > bytes.size() - at)
        return std::nullopt;
    const TimBlock block{at, u16_at(bytes, at + 4), u16_at(bytes, at + 6), u16_at(bytes, at + 8),
                         u16_at(bytes, at + 10)};
    return std::pair{block, at + length};
}

// The TIM starting at `at`; nothing if its headers do not hold together.
std::optional<TimPicture> tim_picture(Bytes bytes, std::size_t at) {
    if (at > bytes.size() || bytes.size() - at < tim_header_bytes || u32_at(bytes, at) != tim_magic)
        return std::nullopt;
    TimPicture picture;
    picture.at = at;
    const std::uint32_t flags = u32_at(bytes, at + 4);
    picture.depth = flags & tim_depth_mask;
    std::size_t next = at + tim_header_bytes;
    if (flags & tim_has_palette) {
        const auto palette = tim_block(bytes, next);
        if (!palette)
            return std::nullopt;
        picture.palette = palette->first;
        next = palette->second;
    }
    const auto image = tim_block(bytes, next);
    if (!image)
        return std::nullopt;
    picture.image = image->first;
    picture.size = image->second - at;
    return picture;
}

// Every TIM of a file of TIMs one after another; nothing unless they fill it exactly.
std::optional<std::vector<TimPicture>> tim_pictures(Bytes bytes) {
    std::vector<TimPicture> pictures;
    std::size_t at = 0;
    while (at < bytes.size()) {
        const auto picture = tim_picture(bytes, at);
        if (!picture)
            return std::nullopt;
        pictures.push_back(*picture);
        at += picture->size;
    }
    return pictures;
}

const PicturePlace *picture_place(std::string_view path) {
    const auto found = std::find_if(picture_places.begin(), picture_places.end(),
                                    [&](const PicturePlace &place) { return place.path == path; });
    return found == picture_places.end() ? nullptr : &*found;
}

std::optional<std::vector<std::uint8_t>> disc_file_bytes(std::string_view path) {
    const std::string disc_path(path);
    DataFile file{};
    if (data_file_open_disc(&file, disc_path.c_str()) != FileResult::Ok)
        return std::nullopt;
    std::vector<std::uint8_t> bytes;
    const FileResult read = data_file_read(&file, bytes, file.size);
    data_file_close(&file);
    if (read != FileResult::Ok)
        return std::nullopt;
    return bytes;
}

// The disc's picture at a place, in the disc's file.
std::optional<TimPicture> disc_picture(Bytes disc, const PicturePlace &place) {
    auto pictures = tim_pictures(disc);
    if (!pictures || place.index >= pictures->size())
        return std::nullopt;
    return (*pictures)[place.index];
}

// The replacement as one TIM that can take the disc picture's place, or why not: it is moved to the
// disc's positions, so it needs the same depth and picture size, and a palette no wider and no
// taller exactly when the disc's has one, and it may be no larger than the disc's picture.
std::variant<TimPicture, std::string> fitted_picture(Bytes replacement, const TimPicture &disc) {
    const auto parsed = tim_picture(replacement, 0);
    if (!parsed || parsed->size != replacement.size())
        return std::string("is not one TIM");
    const TimPicture &picture = *parsed;
    if (picture.size > disc.size)
        return "is larger than the disc's picture (" + std::to_string(picture.size) + " > " +
               std::to_string(disc.size) + " bytes)";
    if (picture.depth != disc.depth)
        return std::string("has another colour depth than the disc's picture");
    if (picture.image.width != disc.image.width || picture.image.height != disc.image.height)
        return "is not the disc's picture's size (" + std::to_string(disc.image.width) + "x" +
               std::to_string(disc.image.height) + " words)";
    if (picture.palette.has_value() != disc.palette.has_value())
        return std::string(disc.palette ? "has no palette; the disc's picture has one"
                                        : "has a palette; the disc's picture has none");
    // Each side: a wider or taller palette would write into another picture's colours.
    if (picture.palette && disc.palette &&
        (picture.palette->width > disc.palette->width || picture.palette->height > disc.palette->height))
        return "has a larger palette than the disc's picture (" + std::to_string(disc.palette->width) + "x" +
               std::to_string(disc.palette->height) + " at most)";
    return picture;
}

// The replacement with its blocks at the disc picture's places in the picture store.
std::vector<std::uint8_t> picture_moved(Bytes replacement, const TimPicture &picture, const TimPicture &disc) {
    std::vector<std::uint8_t> moved(replacement.begin(), replacement.end());
    const auto place = [&](const TimBlock &to, const TimBlock &from) {
        moved[to.at + 4] = static_cast<std::uint8_t>(from.x & 0xff);
        moved[to.at + 5] = static_cast<std::uint8_t>(from.x >> 8);
        moved[to.at + 6] = static_cast<std::uint8_t>(from.y & 0xff);
        moved[to.at + 7] = static_cast<std::uint8_t>(from.y >> 8);
    };
    if (picture.palette && disc.palette)
        place(*picture.palette, *disc.palette);
    place(picture.image, disc.image);
    return moved;
}

// The replacement moved into the disc picture's place; nothing if it does not fit there.
std::optional<std::vector<std::uint8_t>> moved_if_fits(Bytes replacement, const TimPicture &disc) {
    const auto fitted = fitted_picture(replacement, disc);
    const auto *picture = std::get_if<TimPicture>(&fitted);
    if (!picture)
        return std::nullopt;
    return picture_moved(replacement, *picture, disc);
}

Refusal check_picture(std::string_view path, Bytes bytes) {
    if (auto reason = check_tim(bytes))
        return reason;
    const PicturePlace *place = picture_place(path);
    const auto disc = place ? disc_file_bytes(place->file) : std::nullopt;
    const auto picture = disc ? disc_picture(*disc, *place) : std::nullopt;
    if (!picture)
        return std::string("is not a picture on the disc");
    const auto fitted = fitted_picture(bytes, *picture);
    if (const auto *reason = std::get_if<std::string>(&fitted))
        return *reason;
    return std::nullopt;
}

} // namespace

std::optional<std::string> mod_tim_refusal(std::span<const std::uint8_t> bytes) {
    return check_tim(bytes);
}

std::optional<std::string> mod_seq_refusal(std::span<const std::uint8_t> bytes) {
    return check_seq(bytes);
}

std::optional<std::string> mod_tmd_refusal(std::span<const std::uint8_t> tmd) {
    return tmd_refusal(tmd);
}

std::vector<verdite::ModAssetKind> mod_asset_kinds() {
    return {asset_kinds.begin(), asset_kinds.end()};
}

std::optional<std::size_t> mod_disc_file_size(std::string_view path) {
    if (path.find('\0') != std::string_view::npos)
        return std::nullopt;
    // A picture inside a file: its file's size. Its check measures it against the disc's picture,
    // reading the file once.
    if (const PicturePlace *place = picture_place(path))
        return mod_disc_file_size(place->file);
    const std::string disc_path(path);
    DataFile file{};
    if (data_file_open_disc(&file, disc_path.c_str()) != FileResult::Ok)
        return std::nullopt;
    const std::size_t size = file.size;
    data_file_close(&file);
    return size;
}

std::optional<std::vector<std::uint8_t>> mod_disc_file_at(const char *directory, std::string_view path) {
    if (!directory || path.find('\0') != std::string_view::npos)
        return std::nullopt;
    const std::string disc_path(path);
    DataFile file{};
    if (data_file_open_at(&file, directory, disc_path.c_str()) != FileResult::Ok)
        return std::nullopt;
    std::vector<std::uint8_t> bytes;
    const FileResult read = data_file_read(&file, bytes, file.size);
    data_file_close(&file);
    if (read != FileResult::Ok)
        return std::nullopt;
    return bytes;
}

std::vector<std::string_view> mod_pictures_in(std::string_view file) {
    std::vector<std::string_view> paths;
    for (const PicturePlace &place : picture_places)
        if (place.file == file)
            paths.push_back(place.path);
    return paths;
}

std::optional<std::vector<std::uint8_t>> mod_pictures_put_in(std::string_view file, std::span<const std::uint8_t> disc,
                                                             const PictureLookup &replacement_of) {
    const auto pictures = tim_pictures(disc);
    if (!pictures)
        return std::nullopt;
    // Each picture's bytes in the file to give the game: the disc's, or a replacement moved into place.
    std::vector<std::optional<std::vector<std::uint8_t>>> moved(pictures->size());
    bool any = false;
    for (const PicturePlace &place : picture_places) {
        if (place.file != file || place.index >= pictures->size())
            continue;
        const auto replacement = replacement_of(place.path);
        // Checked when the mod set it; checked again, against the disc's file the game is loading.
        if (replacement)
            moved[place.index] = moved_if_fits(*replacement, (*pictures)[place.index]);
        any = any || moved[place.index].has_value();
    }
    if (!any)
        return std::nullopt;
    std::vector<std::uint8_t> out;
    out.reserve(disc.size());
    for (std::size_t i = 0; i < pictures->size(); ++i) {
        if (const auto &bytes = moved[i]) {
            out.insert(out.end(), bytes->begin(), bytes->end());
        } else {
            const TimPicture &picture = (*pictures)[i];
            const auto from = disc.begin() + static_cast<std::ptrdiff_t>(picture.at);
            out.insert(out.end(), from, from + static_cast<std::ptrdiff_t>(picture.size));
        }
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> mod_picture_for_upload(std::string_view path, std::span<const std::uint8_t> disc,
                                                                 const std::vector<std::uint8_t> *replacement) {
    const PicturePlace *place = picture_place(path);
    const auto picture = place ? disc_picture(disc, *place) : std::nullopt;
    if (!picture)
        return std::nullopt;
    if (replacement)
        if (auto moved = moved_if_fits(*replacement, *picture))
            return moved;
    const auto from = disc.begin() + static_cast<std::ptrdiff_t>(picture->at);
    return std::vector<std::uint8_t>(from, from + static_cast<std::ptrdiff_t>(picture->size));
}

} // namespace kf::ext
