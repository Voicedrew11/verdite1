#include <kf/renderer/backend.h>

namespace kf {
namespace {
// Headless runs: takes everything, draws nothing.
class NullBackend final : public RenderBackend {
public:
    Tier tier() const override { return Tier::Enhanced; }
    const char *name() const override { return "null"; }
    bool set_target(int, int) override { return true; }
    void set_options(const PictureOptions &) override {}
    void upload_layer(u8, const Image &) override {}
    void set_occluders(std::span<const u8>, int, int, float) override {}
    void upload_frame(std::span<const GpuVertex>, std::span<const DrawRun>, const FrameStyle &) override {}
    void present(const FrameView &, std::span<const RenderLight>, int, int, const ImDrawData *) override {}
    std::optional<Image> capture() override { return std::nullopt; }
};
} // namespace

std::unique_ptr<RenderBackend> make_null_backend() { return std::make_unique<NullBackend>(); }
} // namespace kf
