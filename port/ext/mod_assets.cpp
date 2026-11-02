#include "mod_assets.h"

#include <kf/audio/codec.h>
#include <kf/lib/codec.h>
#include <kf/lib/tmd.h>
#include <kf/platform/files.h>
#include <kf/renderer/constants.h>

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
#include <vector>

namespace kf::ext {
namespace {

using Bytes = std::span<const std::uint8_t>;
using Refusal = std::optional<std::string>;

Refusal check_tim(Bytes bytes);
Refusal check_seq(Bytes bytes);
Refusal check_tmd(Bytes bytes);

// The kinds a mod may replace, by suffix: a file takes the first row its disc path ends with.
// No MIM row: a MIM with fewer animation clips than the game asks for passes any byte check (docs/MODDING.md).
const std::array<verdite::ModAssetKind, 3> asset_kinds{{
    {".TIM", check_tim},
    {".SEQ", check_seq},
    {".TMD", check_tmd},
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

} // namespace

std::vector<verdite::ModAssetKind> mod_asset_kinds() {
    return {asset_kinds.begin(), asset_kinds.end()};
}

std::optional<std::size_t> mod_disc_file_size(std::string_view path) {
    if (path.find('\0') != std::string_view::npos)
        return std::nullopt;
    const std::string disc_path(path);
    DataFile file{};
    if (data_file_open_disc(&file, disc_path.c_str()) != FileResult::Ok)
        return std::nullopt;
    const std::size_t size = file.size;
    data_file_close(&file);
    return size;
}

} // namespace kf::ext
