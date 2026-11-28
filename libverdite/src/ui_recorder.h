#ifndef VERDITE_UI_RECORDER_H
#define VERDITE_UI_RECORDER_H
// RmlUi's drawing recorded into a verdite::ui::Frame (verdite/ui_frame.h).
// From holyland's port/ui/ui_render.cpp. Only what the style sheets use:
// triangles, textures and scissor clipping. Transforms, clip masks, layers,
// filters and shaders are not drawn (RmlUi then draws without them); the first
// use of each is logged once. Game thread.
#include <RmlUi/Core/RenderInterface.h>

#include <cstdint>
#include <functional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <verdite/ui.h>

namespace verdite::ui {

class Recorder final : public Rml::RenderInterface {
public:
    using Log = std::function<void(std::string_view)>;
    Recorder(const ImageSource *images, Log log);

    // Starts a picture: the last frame's draws, uploads and releases are
    // emptied, and those made or let go of between frames join this one.
    void begin_frame(int width_px, int height_px);
    void end_frame();
    [[nodiscard]] const Frame &frame() const { return frame_; }

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                        Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;
    // The ImageSource's images; others are not drawn (0).
    Rml::TextureHandle LoadTexture(Rml::Vector2i &dimensions, const Rml::String &source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source,
                                       Rml::Vector2i dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;
    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;
    void SetTransform(const Rml::Matrix4f *transform) override;
    void EnableClipMask(bool enable) override;

private:
    TextureId add_upload(TextureUpload upload);
    void not_drawn(std::string_view what);

    struct Geometry {
        std::vector<Vertex> vertices;
        std::vector<std::uint32_t> indices;
    };
    const ImageSource *images_;
    Log log_;
    std::vector<std::string_view> told_;
    std::unordered_map<std::uintptr_t, Geometry> geometries_;
    std::uintptr_t next_geometry_ = 1;
    TextureId next_texture_ = 1;
    Frame frame_;
    bool recording_ = false;
    std::vector<TextureUpload> held_uploads_;
    std::vector<TextureId> held_releases_;
    bool clipping_ = false;
    Rect clip_ = {};
};

} // namespace verdite::ui

#endif // VERDITE_UI_RECORDER_H
