// RmlUi's drawing recorded (ui_recorder.h)
#include "ui_recorder.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

namespace verdite::ui {

Recorder::Recorder(const ImageSource *images, Log log) : images_(images), log_(std::move(log)) {}

void Recorder::not_drawn(std::string_view what) {
    if (std::find(told_.begin(), told_.end(), what) != told_.end()) {
        return;
    }
    told_.push_back(what);
    if (log_) {
        log_(std::string("RmlUi asked for ") + std::string(what) + ", which is not drawn");
    }
}

void Recorder::begin_frame(int width_px, int height_px) {
    frame_.width_px = width_px;
    frame_.height_px = height_px;
    frame_.vertices.clear();
    frame_.indices.clear();
    frame_.draws.clear();
    frame_.uploads = std::move(held_uploads_);
    frame_.releases = std::move(held_releases_);
    held_uploads_.clear();
    held_releases_.clear();
    clipping_ = false;
    recording_ = true;
}

void Recorder::end_frame() {
    recording_ = false;
}

TextureId Recorder::add_upload(TextureUpload upload) {
    upload.id = next_texture_++;
    std::vector<TextureUpload> &to = recording_ ? frame_.uploads : held_uploads_;
    to.push_back(std::move(upload));
    return to.back().id;
}

Rml::CompiledGeometryHandle Recorder::CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                      Rml::Span<const int> indices) {
    static_assert(sizeof(Rml::Vertex) == sizeof(Vertex), "Rml::Vertex is copied as a Vertex");
    Geometry geometry;
    geometry.vertices.resize(vertices.size());
    std::memcpy(geometry.vertices.data(), vertices.data(), vertices.size() * sizeof(Vertex));
    geometry.indices.reserve(indices.size());
    for (int index : indices) {
        // (an index past its own vertices would read past the frame's)
        if (index < 0 || static_cast<size_t>(index) >= vertices.size()) {
            if (log_) {
                log_("a geometry's index is out of its vertices");
            }
            return 0;
        }
        geometry.indices.push_back(static_cast<std::uint32_t>(index));
    }
    std::uintptr_t handle = next_geometry_++;
    geometries_.emplace(handle, std::move(geometry));
    return handle;
}

void Recorder::RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation,
                              Rml::TextureHandle texture) {
    auto found = geometries_.find(handle);
    if (found == geometries_.end() || found->second.indices.empty()) {
        return;
    }
    const Geometry &geometry = found->second;
    Draw draw = {};
    draw.first_index = static_cast<std::uint32_t>(frame_.indices.size());
    draw.index_count = static_cast<std::uint32_t>(geometry.indices.size());
    draw.vertex_offset = static_cast<std::uint32_t>(frame_.vertices.size());
    draw.texture = static_cast<TextureId>(texture);
    draw.translate_x = translation.x;
    draw.translate_y = translation.y;
    draw.clipped = clipping_;
    draw.clip = clip_;
    frame_.vertices.insert(frame_.vertices.end(), geometry.vertices.begin(),
                           geometry.vertices.end());
    frame_.indices.insert(frame_.indices.end(), geometry.indices.begin(), geometry.indices.end());
    frame_.draws.push_back(draw);
}

void Recorder::ReleaseGeometry(Rml::CompiledGeometryHandle handle) {
    geometries_.erase(handle);
}

Rml::TextureHandle Recorder::LoadTexture(Rml::Vector2i &dimensions, const Rml::String &source) {
    dimensions = {0, 0};
    std::optional<TextureUpload> upload;
    if (images_ != nullptr) {
        upload = images_->load(source);
    }
    if (!upload || upload->width_px <= 0 || upload->height_px <= 0 ||
        upload->rgba.size() != static_cast<size_t>(upload->width_px) *
                                   static_cast<size_t>(upload->height_px) * 4) {
        not_drawn("an image the game does not have");
        return 0;
    }
    dimensions = {upload->width_px, upload->height_px};
    return add_upload(std::move(*upload));
}

Rml::TextureHandle Recorder::GenerateTexture(Rml::Span<const Rml::byte> source,
                                             Rml::Vector2i dimensions) {
    if (dimensions.x <= 0 || dimensions.y <= 0 ||
        source.size() != static_cast<size_t>(dimensions.x) * static_cast<size_t>(dimensions.y) * 4) {
        return 0;
    }
    TextureUpload upload;
    upload.width_px = dimensions.x;
    upload.height_px = dimensions.y;
    upload.rgba.assign(source.begin(), source.end());
    return add_upload(std::move(upload));
}

void Recorder::ReleaseTexture(Rml::TextureHandle texture) {
    if (texture != 0) {
        (recording_ ? frame_.releases : held_releases_).push_back(static_cast<TextureId>(texture));
    }
}

void Recorder::EnableScissorRegion(bool enable) {
    clipping_ = enable;
}

void Recorder::SetScissorRegion(Rml::Rectanglei region) {
    clip_ = {region.Left(), region.Top(), region.Width(), region.Height()};
}

void Recorder::SetTransform(const Rml::Matrix4f *transform) {
    if (transform != nullptr) {
        not_drawn("a transform");
    }
}

void Recorder::EnableClipMask(bool enable) {
    if (enable) {
        not_drawn("a clip mask");
    }
}

} // namespace verdite::ui
