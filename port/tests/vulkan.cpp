// The Vulkan backend drawn offscreen on a real GPU, its pixels checked against
// the PS1's arithmetic. Exits 77 (skipped) when no Vulkan device is available.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <kf/renderer/backend.h>

#include "vulkan_device.h"
#include "vulkan_downsample.h"
#include "vulkan_resources.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace kf;

namespace {
RenderBackend *backend = nullptr;

constexpr int band_height = 40;
// Texture columns: 0-15 opaque, 16-31 STP, 32-47 transparent.
constexpr int opaque_columns = 16, stp_columns = 32, texture_columns = 48;
struct Rgb5 {
    int r, g, b;
    bool operator==(const Rgb5 &) const = default;
};
constexpr Rgb5 opaque_texel{10, 20, 5};
constexpr Rgb5 stp_texel{4, 4, 4};
constexpr Rgb5 background{20, 20, 20};

u8 expand5(int channel) { return static_cast<u8>((channel << 3) | (channel >> 2)); }

Image test_page() {
    Image page{texture_page_extent, texture_page_extent, {}};
    page.rgba.assign(std::size_t{texture_page_extent} * texture_page_extent * 4, 0);
    for (int y = 0; y < texture_page_extent; ++y) {
        for (int x = 0; x < texture_columns; ++x) {
            const bool stp = x >= opaque_columns && x < stp_columns;
            if (x >= stp_columns)
                continue;   // transparent: all zero
            const Rgb5 c = stp ? stp_texel : opaque_texel;
            u8 *pixel = &page.rgba[(std::size_t{static_cast<unsigned>(y)} * texture_page_extent + x) * 4];
            pixel[0] = expand5(c.r);
            pixel[1] = expand5(c.g);
            pixel[2] = expand5(c.b);
            pixel[3] = stp ? 128 : 255;
        }
    }
    return page;
}

GpuVertex corner(float x, float y, float u, float v, VertexTexturing texturing, std::array<u8, 4> color) {
    GpuVertex out{};
    out.x = x;
    out.y = y;
    out.u = u / texture_page_extent;
    out.v = v / texture_page_extent;
    out.uv_bounds = {0, 0, texture_columns, texture_columns};
    out.color = color;
    out.texturing = texturing;
    return out;
}

// A 1:1 textured (or solid) rectangle as two triangles.
void add_rect(std::vector<GpuVertex> &vertices, int x, int y, int width, int height, VertexTexturing texturing,
              std::array<u8, 4> color = {128, 128, 128, 255}) {
    const auto fx = static_cast<float>(x), fy = static_cast<float>(y);
    const auto fw = static_cast<float>(width), fh = static_cast<float>(height);
    const GpuVertex a = corner(fx, fy, 0, 0, texturing, color);
    const GpuVertex b = corner(fx + fw, fy, fw, 0, texturing, color);
    const GpuVertex c = corner(fx, fy + fh, 0, fh, texturing, color);
    const GpuVertex d = corner(fx + fw, fy + fh, fw, fh, texturing, color);
    vertices.insert(vertices.end(), {a, b, c, b, d, c});
}

FrameStyle cleared_to(Rgb5 color) {
    FrameStyle style;
    style.red = expand5(color.r) / 255.0f;
    style.green = expand5(color.g) / 255.0f;
    style.blue = expand5(color.b) / 255.0f;
    return style;
}

FrameView unmoved_view(const std::vector<Motion> &motions) {
    return {motions, false, false, 200, 160, 120, {1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}};
}

// Presents the uploaded frame again and returns the picture.
Image present_and_capture() {
    static const std::vector<Motion> motions(1, Motion{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}});
    backend->present(unmoved_view(motions), {}, 0, 0, nullptr);
    std::optional<Image> picture = backend->capture();
    if (!picture) {
        FAIL("nothing captured");
        return {};
    }
    REQUIRE(picture->width == 320);
    return std::move(*picture);
}

// Draws one frame and returns the picture.
Image draw(const std::vector<GpuVertex> &vertices, const std::vector<DrawRun> &runs, const FrameStyle &style) {
    backend->upload_frame(vertices, runs, style);
    return present_and_capture();
}

Rgb5 pixel(const Image &image, int x, int y) {
    const u8 *p = &image.rgba[(std::size_t{static_cast<unsigned>(y)} * image.width + x) * 4];
    return {p[0] >> 3, p[1] >> 3, p[2] >> 3};
}

int widest_step(Rgb5 a, Rgb5 b) {
    return std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)});
}

DrawRun run_of(BlendMode blend, std::size_t first, std::size_t count) {
    return {{blend, false, false}, static_cast<u32>(first), static_cast<u32>(count)};
}

// A test pattern: no two pixels of a block alike, so a wrong tap shows.
u8 pattern_channel(int x, int y, int channel) { return static_cast<u8>((x * 37 + y * 11 + channel * 50) % 256); }

// Averages a pattern of this size through Downsample on its own device and
// returns the picture, RGBA8.
std::vector<u8> downsampled_pattern(const VulkanDevice &device, uint32_t width, uint32_t height, uint32_t factor) {
    std::unique_ptr<Downsample> downsample = Downsample::create(device);
    REQUIRE(downsample);
    const ImageShape shape{width, height, 1, VK_FORMAT_R8G8B8A8_UNORM,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT};
    const GpuImage target = GpuImage::create(device.device(), device.allocator(), shape);
    REQUIRE(target);
    REQUIRE(downsample->fit(target, factor));
    GpuBuffer upload = GpuBuffer::create(device.allocator(), VkDeviceSize{width} * height * 4,
                                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT, BufferAccess::HostWrites);
    const uint32_t shown_width = width / factor, shown_height = height / factor;
    GpuBuffer readback = GpuBuffer::create(device.allocator(), VkDeviceSize{shown_width} * shown_height * 4,
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferAccess::HostReads);
    REQUIRE((upload && upload.data() && readback && readback.data()));
    auto *texels = static_cast<u8 *>(upload.data());
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
            for (int c = 0; c < 4; ++c)
                texels[(y * width + x) * 4 + c] = c == 3 ? 255 : pattern_channel(static_cast<int>(x), static_cast<int>(y), c);
    upload.flush();
    const bool ran = device.run_now([&](VkCommandBuffer commands) {
        transition(commands, target.image(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(commands, upload.buffer(), target.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        transition(commands, target.image(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        downsample->record(commands);
        copy.imageExtent = {shown_width, shown_height, 1};
        vkCmdCopyImageToBuffer(commands, downsample->picture().image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback.buffer(), 1, &copy);
    });
    REQUIRE(ran);
    readback.invalidate();
    std::vector<u8> out(std::size_t{shown_width} * shown_height * 4);
    std::memcpy(out.data(), readback.data(), out.size());
    return out;
}
} // namespace

TEST_CASE("vulkan: supersampling shows the mean of each N x N block") {
    VulkanDeviceStart started = VulkanDevice::create(nullptr);
    REQUIRE(started.device);
    const VulkanDevice &device = *started.device;
    constexpr uint32_t width = 12, height = 6;
    for (const uint32_t factor : {2u, 3u}) {
        CAPTURE(factor);
        const std::vector<u8> picture = downsampled_pattern(device, width, height, factor);
        const uint32_t shown_width = width / factor;
        for (uint32_t y = 0; y < height / factor; ++y) {
            for (uint32_t x = 0; x < shown_width; ++x) {
                for (int c = 0; c < 3; ++c) {
                    int sum = 0;
                    for (uint32_t j = 0; j < factor; ++j)
                        for (uint32_t i = 0; i < factor; ++i)
                            sum += pattern_channel(static_cast<int>(x * factor + i), static_cast<int>(y * factor + j), c);
                    const double mean = static_cast<double>(sum) / (factor * factor);
                    CAPTURE(x);
                    CAPTURE(y);
                    CHECK(std::abs(picture[(y * shown_width + x) * 4 + c] - mean) <= 0.51);
                }
            }
        }
    }
    CHECK(device.validation_errors() == 0);
}

TEST_CASE("vulkan: the clear colour is cut to 15 bits") {
    FrameStyle style;
    style.red = 1.0f;
    style.green = 0.5f;
    const Image picture = draw({}, {}, style);
    CHECK(pixel(picture, 10, 10) == Rgb5{31, 16, 0});
}

TEST_CASE("vulkan: opaque texels replace, STP texels blend, transparent texels are skipped") {
    // One band per mode; raw texturing, so a texel's colour is its own.
    const BlendMode modes[] = {BlendMode::average, BlendMode::add, BlendMode::subtract, BlendMode::add_quarter};
    const Rgb5 blended[] = {{12, 12, 12}, {24, 24, 24}, {16, 16, 16}, {21, 21, 21}};
    std::vector<GpuVertex> vertices;
    std::vector<DrawRun> runs;
    for (int band = 0; band < 4; ++band) {
        const std::size_t first = vertices.size();
        add_rect(vertices, 0, band * band_height, texture_columns, band_height, VertexTexturing::Raw);
        runs.push_back(run_of(modes[band], first, vertices.size() - first));
    }
    const Image picture = draw(vertices, runs, cleared_to(background));
    for (int band = 0; band < 4; ++band) {
        CAPTURE(band);
        const int y = band * band_height + band_height / 2;
        CHECK(pixel(picture, 8, y) == opaque_texel);
        CHECK(pixel(picture, 24, y) == blended[band]);
        CHECK(pixel(picture, 40, y) == background);
    }
}

TEST_CASE("vulkan: modulation at unity and flat shading are exact") {
    std::vector<GpuVertex> vertices;
    add_rect(vertices, 0, 0, texture_columns, band_height, VertexTexturing::Modulated);
    add_rect(vertices, 100, 0, 20, 20, VertexTexturing::Solid, {200, 100, 50, 255});
    const std::vector<DrawRun> runs = {run_of(BlendMode::opaque, 0, vertices.size())};
    const Image picture = draw(vertices, runs, cleared_to(background));
    CHECK(pixel(picture, 8, 10) == opaque_texel);
    CHECK(pixel(picture, 24, 10) == stp_texel);   // opaque face: STP means nothing
    CHECK(pixel(picture, 110, 10) == Rgb5{25, 12, 6});
}

TEST_CASE("vulkan: dither follows the PS1's 4 x 4 pattern on the original pixel grid") {
    std::vector<GpuVertex> vertices;
    // 99 + the offsets straddles a 15-bit step: 95-97 are 11, 98-102 are 12.
    add_rect(vertices, 0, 0, 8, 8, VertexTexturing::Solid, {99, 99, 99, 255});
    for (GpuVertex &v : vertices)
        v.dither = 1;
    const std::vector<DrawRun> runs = {run_of(BlendMode::opaque, 0, vertices.size())};
    const Image picture = draw(vertices, runs, cleared_to(background));
    const int offsets[16] = {-4, 0, -3, 1, 2, -2, 3, -1, -3, 1, -4, 0, 3, -1, 2, -2};
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const int expected = (99 + offsets[y * 4 + x]) >> 3;
            CHECK(pixel(picture, x, y) == Rgb5{expected, expected, expected});
        }
    }
}

TEST_CASE("vulkan: averaging an odd sum is at most one 15-bit step from the PS1") {
    // The decision of 2026-10-23: hardware rounding against the PS1's truncation.
    std::vector<GpuVertex> vertices;
    add_rect(vertices, 0, 0, 20, 20, VertexTexturing::Solid, {8 * 7, 8 * 7, 8 * 7, 255});
    const std::vector<DrawRun> runs = {run_of(BlendMode::average, 0, vertices.size())};
    const Image picture = draw(vertices, runs, cleared_to(background));
    const int truncated = (background.r + 7) >> 1;
    CHECK(widest_step(pixel(picture, 10, 10), Rgb5{truncated, truncated, truncated}) <= 1);
}

TEST_CASE("vulkan: a retained frame is drawn over once, not on every present") {
    std::vector<GpuVertex> vertices;
    add_rect(vertices, 0, 0, 20, 20, VertexTexturing::Solid, {8 * 4, 8 * 4, 8 * 4, 255});
    const std::vector<DrawRun> runs = {run_of(BlendMode::add, 0, vertices.size())};
    draw({}, {}, cleared_to(background));
    FrameStyle keep;
    keep.clear = FrameClear::Retain;
    draw(vertices, runs, keep);
    CHECK(pixel(present_and_capture(), 10, 10) == Rgb5{24, 24, 24});
}

int main(int argc, char **argv) {
    BackendStart started = make_vulkan_backend(nullptr);
    if (!started.backend) {
        std::printf("skipped: %s\n", started.error.c_str());
        return 77;
    }
    backend = started.backend.get();
    if (!backend->set_target(320, 1))
        return 1;
    backend->upload_layer(0, test_page());
    doctest::Context context(argc, argv);
    const int result = context.run();
    started.backend.reset();
    return result;
}
