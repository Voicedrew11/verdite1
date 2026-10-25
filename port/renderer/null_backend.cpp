#include <kf/renderer/backend.h>

#include <cstddef>

namespace kf {
namespace {
// Headless runs: takes everything, draws nothing. Its picture is black at the
// target's size, so a page that captures and restores it works.
class NullBackend final : public RenderBackend {
public:
    Tier tier() const override { return Tier::Enhanced; }
    const char *name() const override { return "null"; }
    PictureOptions drawable(const PictureOptions &wanted) const override { return wanted; }
    bool set_target(int logical_width, int scale) override {
        width_ = static_cast<u32>(logical_width * scale);
        height_ = static_cast<u32>(render_height * scale);
        return true;
    }
    void set_options(const PictureOptions &) override {}
    void upload_layer(u8, const Image &) override {}
    void set_occluders(std::span<const u8>, int, int, float) override {}
    void upload_frame(std::span<const GpuVertex>, std::span<const DrawRun>, const FrameStyle &, const DepthCue &) override {}
    void present(const FrameView &, std::span<const RenderLight>, int, int, ImDrawData *) override {}
    std::optional<Image> capture() override {
        Image picture{width_, height_, {}};
        picture.rgba.resize(static_cast<std::size_t>(width_) * height_ * 4);
        return picture;
    }
    bool restore(const Image &picture) override { return picture.width == width_ && picture.height == height_; }
    bool start_overlay() override { return true; }
    void stop_overlay() override {}
    void release_window() override {}

private:
    u32 width_ = 0, height_ = 0;
};
} // namespace

std::unique_ptr<RenderBackend> make_null_backend() { return std::make_unique<NullBackend>(); }
} // namespace kf
