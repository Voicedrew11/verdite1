// The Vulkan backend drawn offscreen on a real GPU, its pixels checked against
// the PS1's arithmetic. Exits 77 (skipped) when no Vulkan device is available.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <kf/renderer/backend.h>
#include <kf/renderer/lighting.h>
#include <kf/renderer/projection.h>

#include "vulkan_device.h"
#include "vulkan_downsample.h"
#include "vulkan_resources.h"

#include <algorithm>
#include <cmath>
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
    return {motions, 0, false, false, 200, 160, 120, {1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}};
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
    backend->upload_frame(vertices, runs, style, {});
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

namespace {
constexpr float test_projection = 200;

// A world corner in camera space, projected as the game does (distance 200,
// centre 160, 120), flat-shaded grey.
GpuVertex world_corner(float cx, float cy, float cz) {
    GpuVertex out{};
    out.x = cx * test_projection / cz + 160;
    out.y = cy * test_projection / cz + 120;
    out.w = cz;
    out.cx = cx;
    out.cy = cy;
    out.cz = cz;
    out.color = {200, 200, 200, 255};
    out.texturing = VertexTexturing::Solid;
    return out;
}

// A wall facing the camera 2000 units away, and a floor 500 below the eye
// running up to its foot: the crease is on row 170.
std::vector<GpuVertex> wall_and_floor() {
    const GpuVertex wall[4] = {world_corner(-2000, -2000, 2000), world_corner(2000, -2000, 2000),
                               world_corner(-2000, 500, 2000), world_corner(2000, 500, 2000)};
    const GpuVertex floor[4] = {world_corner(-2000, 500, 2000), world_corner(2000, 500, 2000),
                                world_corner(-2000, 500, 600), world_corner(2000, 500, 600)};
    std::vector<GpuVertex> out;
    for (const GpuVertex *q : {wall, floor})
        out.insert(out.end(), {q[0], q[1], q[2], q[1], q[3], q[2]});
    return out;
}

void set_occlusion(float strength) {
    PictureOptions options;
    options.ambient_occlusion = strength;
    options.occlusion_quality = OcclusionQuality::High;
    backend->set_options(options);
}

int brightness(Rgb5 c) { return c.r + c.g + c.b; }
} // namespace

TEST_CASE("vulkan: ambient occlusion darkens the crease and nothing far from it") {
    const std::vector<GpuVertex> vertices = wall_and_floor();
    const std::vector<DrawRun> runs = {run_of(BlendMode::opaque, 0, vertices.size())};
    set_occlusion(0);
    const Image plain = draw(vertices, runs, cleared_to(background));
    set_occlusion(1);
    const Image occluded = draw(vertices, runs, cleared_to(background));
    set_occlusion(0);
    for (const int x : {60, 160, 260}) {
        CAPTURE(x);
        // Wall just above the crease and floor just below it: darker.
        CHECK(brightness(pixel(occluded, x, 166)) < brightness(pixel(plain, x, 166)));
        CHECK(brightness(pixel(occluded, x, 174)) < brightness(pixel(plain, x, 174)));
        // Wall 1300 units above the floor and floor 1000 out from the wall: out of reach.
        CHECK(pixel(occluded, x, 40) == pixel(plain, x, 40));
        CHECK(pixel(occluded, x, 235) == pixel(plain, x, 235));
    }
}

TEST_CASE("vulkan: ambient occlusion leaves 2D faces over the world alone") {
    std::vector<GpuVertex> vertices = wall_and_floor();
    const std::size_t world = vertices.size();
    add_rect(vertices, 100, 150, 120, 40, VertexTexturing::Solid, {200, 200, 200, 255});
    const std::vector<DrawRun> runs = {run_of(BlendMode::opaque, 0, world),
                                       run_of(BlendMode::opaque, world, vertices.size() - world)};
    set_occlusion(0);
    const Image plain = draw(vertices, runs, cleared_to(background));
    set_occlusion(2);
    const Image occluded = draw(vertices, runs, cleared_to(background));
    set_occlusion(0);
    CHECK(pixel(occluded, 160, 170) == pixel(plain, 160, 170));
    CHECK(brightness(pixel(occluded, 60, 168)) < brightness(pixel(plain, 60, 168)));
}

namespace {
constexpr float wall_depth = 14000;
constexpr u8 wall_clear = 200;

// A wall across the view at wall_depth, cued by the game at scale (the map's
// 2), its cued colour black so a pixel drawn with it shows.
std::vector<GpuVertex> cued_wall(u8 scale_quarters) {
    const float half_width = 30000, half_height = 20000;
    std::vector<GpuVertex> out;
    const GpuVertex corners[4] = {
        world_corner(-half_width, -half_height, wall_depth), world_corner(half_width, -half_height, wall_depth),
        world_corner(-half_width, half_height, wall_depth), world_corner(half_width, half_height, wall_depth)};
    out.insert(out.end(), {corners[0], corners[1], corners[2], corners[1], corners[3], corners[2]});
    for (GpuVertex &vertex : out) {
        vertex.color = {0, 0, 0, 255};
        vertex.clear_color = {wall_clear, wall_clear, wall_clear};
        vertex.cue_scale_quarters = scale_quarters;
    }
    return out;
}

// The game's cued grey for a point straight ahead at this depth, as a corner
// there would get it, in 5 bits.
int game_cued(float depth, int scale) {
    MATRIX identity{};
    identity.m[0][0] = identity.m[1][1] = identity.m[2][2] = 4096;
    const SVECTOR ahead{0, 0, static_cast<s16>(depth), 0};
    const ProjectedPoint point = render_project_point(identity, Projection{}, ahead);
    const LightingEnvironment black{};
    const CVECTOR clear{wall_clear, wall_clear, wall_clear, 0};
    return render_fog_color(black, clear, point.fog * scale).r >> 3;
}

Image draw_fogged(const std::vector<GpuVertex> &vertices, FogMode fog, float enhanced_depth = 0) {
    PictureOptions options;
    options.fog = fog;
    options.enhanced_depth = enhanced_depth;
    backend->set_options(options);
    DepthCue cue;
    cue.slope = render_fog_slope(Projection{});
    const std::vector<DrawRun> runs = {run_of(BlendMode::opaque, 0, vertices.size())};
    backend->upload_frame(vertices, runs, cleared_to(background), cue);
    Image picture = present_and_capture();
    backend->set_options({});
    return picture;
}
} // namespace

TEST_CASE("vulkan: fog per pixel by depth is the game's cue at the corners' depth") {
    const Image picture = draw_fogged(cued_wall(8), FogMode::Depth);
    const int expected = game_cued(wall_depth, 2);
    REQUIRE(expected > 0);
    REQUIRE(expected < wall_clear >> 3);
    for (const int x : {20, 160, 300}) {
        CAPTURE(x);
        CHECK(std::abs(pixel(picture, x, 120).r - expected) <= 1);
        CHECK(pixel(picture, x, 120) == pixel(picture, x, 30));
    }
}

TEST_CASE("vulkan: fog per pixel by distance deepens off the view's centre") {
    const Image picture = draw_fogged(cued_wall(8), FogMode::Distance);
    CHECK(std::abs(pixel(picture, 160, 120).r - game_cued(wall_depth, 2)) <= 1);
    // 140 columns right of the centre: 140 x 14000 / 200 = 9800 units across.
    const float off_centre = std::hypot(9800.0f, wall_depth);
    CHECK(std::abs(pixel(picture, 300, 120).r - game_cued(off_centre, 2)) <= 1);
    CHECK(pixel(picture, 300, 120).r < pixel(picture, 160, 120).r);
}

TEST_CASE("vulkan: fog keeps each face's strength, and faces the game did not cue keep their colour") {
    const Image sprite = draw_fogged(cued_wall(4), FogMode::Depth);
    CHECK(std::abs(pixel(sprite, 160, 120).r - game_cued(wall_depth, 1)) <= 1);
    const Image uncued = draw_fogged(cued_wall(0), FogMode::Distance);
    CHECK(pixel(uncued, 160, 120) == Rgb5{0, 0, 0});
}

TEST_CASE("vulkan: fog at the corners draws the game's own colours") {
    const Image picture = draw_fogged(cued_wall(8), FogMode::Corners);
    CHECK(pixel(picture, 160, 120) == Rgb5{0, 0, 0});
}

namespace {
// Layer 1: a one-texel checker of black and grey 30 in the aligned square
// 64..95, red 96..127 beside it, white over the left half of 128..159 with the
// right half transparent, and stripes 4 texels wide, black then grey, across 160..191.
constexpr u8 mip_layer = 1;
constexpr int checker_low = 64, square_size = 32, red_low = 96, cutout_low = 128, stripes_low = 160;
constexpr Rgb5 checker_grey{30, 30, 30};

Image mip_test_page() {
    Image page{texture_page_extent, texture_page_extent, {}};
    page.rgba.assign(std::size_t{texture_page_extent} * texture_page_extent * 4, 0);
    const auto put = [&](int x, int y, Rgb5 c) {
        u8 *texel = &page.rgba[(std::size_t{static_cast<unsigned>(y)} * texture_page_extent + x) * 4];
        texel[0] = expand5(c.r);
        texel[1] = expand5(c.g);
        texel[2] = expand5(c.b);
        texel[3] = 255;
    };
    for (int y = 0; y < texture_page_extent; ++y) {
        for (int x = 0; x < square_size; ++x) {
            put(checker_low + x, y, (x + y) % 2 ? checker_grey : Rgb5{0, 0, 0});
            put(red_low + x, y, Rgb5{31, 0, 0});
            if (x < square_size / 2)
                put(cutout_low + x, y, Rgb5{31, 31, 31});
            put(stripes_low + x, y, (x / 4) % 2 ? checker_grey : Rgb5{0, 0, 0});
        }
    }
    return page;
}

// A world face 200 units ahead filling width x height pixels about the view's
// centre, showing the 32 x 32 texels from (u, 0).
std::vector<GpuVertex> textured_rect(int u, float width, float height) {
    std::vector<GpuVertex> out;
    const float x = width / 2, y = height / 2;
    GpuVertex corners[4] = {world_corner(-x, -y, 200), world_corner(x, -y, 200), world_corner(-x, y, 200),
                            world_corner(x, y, 200)};
    const float uvs[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    for (int i = 0; i < 4; ++i) {
        GpuVertex &corner = corners[i];
        corner.u = static_cast<float>(u + uvs[i][0] * square_size) / texture_page_extent;
        corner.v = uvs[i][1] * square_size / texture_page_extent;
        corner.uv_bounds = {static_cast<u16>(u), 0, static_cast<u16>(u + square_size), square_size};
        corner.texturing = VertexTexturing::Raw;
        corner.layer = mip_layer;
    }
    out.insert(out.end(), {corners[0], corners[1], corners[2], corners[1], corners[3], corners[2]});
    return out;
}

// 8 x 8 pixels from (156, 116): four texels a pixel.
std::vector<GpuVertex> minified_square(int u) { return textured_rect(u, 8, 8); }

Image draw_textured(const std::vector<GpuVertex> &vertices, bool mipmaps, TextureFilter filter, int anisotropy = 1,
                   float enhanced_depth = 0) {
    PictureOptions options;
    options.mipmaps = mipmaps;
    options.texture_filter = filter;
    options.anisotropy = anisotropy;
    options.enhanced_depth = enhanced_depth;
    backend->set_options(options);
    const Image picture = draw(vertices, {run_of(BlendMode::opaque, 0, vertices.size())}, cleared_to(background));
    backend->set_options({});
    return picture;
}
} // namespace

TEST_CASE("vulkan: mipmaps average a minified face, with no texel from beside its square") {
    const std::vector<GpuVertex> checker = minified_square(checker_low);
    const Image plain = draw_textured(checker, false, TextureFilter::Nearest);
    CHECK(widest_step(pixel(plain, 158, 118), checker_grey) * widest_step(pixel(plain, 158, 118), Rgb5{}) == 0);
    for (const TextureFilter filter :
         {TextureFilter::Nearest, TextureFilter::Bilinear, TextureFilter::ThreePoint, TextureFilter::Sharp}) {
        CAPTURE(static_cast<int>(filter));
        const Image mipped = draw_textured(checker, true, filter);
        for (int y = 116; y < 124; ++y) {
            for (int x = 156; x < 164; ++x) {
                CAPTURE(x);
                CAPTURE(y);
                CHECK(widest_step(pixel(mipped, x, y), Rgb5{15, 15, 15}) <= 1);
            }
        }
    }
}

TEST_CASE("vulkan: a mipmapped cut-out keeps its colour and its edge") {
    const std::vector<GpuVertex> cutout = minified_square(cutout_low);
    for (const TextureFilter filter : {TextureFilter::Nearest, TextureFilter::Bilinear, TextureFilter::Sharp}) {
        CAPTURE(static_cast<int>(filter));
        const Image mipped = draw_textured(cutout, true, filter);
        for (int x = 156; x < 164; ++x) {
            CAPTURE(x);
            // The face's left half is the white texels, its right half transparent.
            CHECK(pixel(mipped, x, 120) == (x < 160 ? Rgb5{31, 31, 31} : background));
        }
    }
}

TEST_CASE("vulkan: anisotropic filtering keeps stripes across a face squeezed the other way") {
    PictureOptions wanted;
    wanted.anisotropy = 16;
    if (backend->drawable(wanted).anisotropy < 16) {
        MESSAGE("skipped: the device has no 16x anisotropic filtering");
        return;
    }
    // 32 x 2 pixels from (144, 119): a texel a pixel across, 16 a pixel down.
    const std::vector<GpuVertex> squeezed = textured_rect(stripes_low, 32, 2);
    for (const TextureFilter filter : {TextureFilter::Nearest, TextureFilter::Bilinear}) {
        CAPTURE(static_cast<int>(filter));
        const Image isotropic = draw_textured(squeezed, true, filter, 1);
        CHECK(widest_step(pixel(isotropic, 145, 120), pixel(isotropic, 149, 120)) <= 2);
        const Image anisotropic = draw_textured(squeezed, true, filter, 16);
        CHECK(widest_step(pixel(anisotropic, 145, 120), Rgb5{0, 0, 0}) <= 2);
        CHECK(widest_step(pixel(anisotropic, 149, 120), checker_grey) <= 2);
    }
}

TEST_CASE("vulkan: nearest mipmaps average a slanted face's texels along its long axis") {
    PictureOptions wanted;
    wanted.anisotropy = 16;
    if (backend->drawable(wanted).anisotropy < 16) {
        MESSAGE("skipped: the device has no 16x anisotropic filtering");
        return;
    }
    // 32 x 2 pixels: a texel a pixel across, 16 a pixel down, as a ceiling seen
    // at a low angle. One tap a pixel would pick a black or grey texel.
    const std::vector<GpuVertex> squeezed = textured_rect(checker_low, 32, 2);
    for (const TextureFilter filter : {TextureFilter::Nearest, TextureFilter::Sharp}) {
        CAPTURE(static_cast<int>(filter));
        const Image mipped = draw_textured(squeezed, true, filter, 16);
        for (int x = 146; x < 174; ++x) {
            CAPTURE(x);
            CHECK(widest_step(pixel(mipped, x, 120), Rgb5{15, 15, 15}) <= 2);
        }
    }
}

TEST_CASE("vulkan: nearest mipmaps leave texels that cover a pixel or more as they were") {
    for (const float extent : {32.0f, 48.0f, 64.0f}) {
        CAPTURE(extent);
        const std::vector<GpuVertex> face = textured_rect(checker_low, extent, extent);
        const Image plain = draw_textured(face, false, TextureFilter::Nearest);
        const Image mipped = draw_textured(face, true, TextureFilter::Nearest, 16);
        const int low_x = 160 - static_cast<int>(extent) / 2, low_y = 120 - static_cast<int>(extent) / 2;
        int differing = 0;
        for (int y = low_y; y < low_y + static_cast<int>(extent); ++y) {
            for (int x = low_x; x < low_x + static_cast<int>(extent); ++x)
                differing += pixel(plain, x, y) == pixel(mipped, x, y) ? 0 : 1;
        }
        CHECK(differing == 0);
    }
}

TEST_CASE("vulkan: sharp filtering draws magnified texels evenly sized, nearest inside them") {
    // 75 pixels from x 122.5, 2.34 a texel: nearest draws the 9.375-pixel grey
    // stripes 9, 9 and 10 pixels wide.
    constexpr float extent = 75, low_x = 160 - extent / 2, stripe_width = extent / 8;
    constexpr int row = 120;
    const std::vector<GpuVertex> face = textured_rect(stripes_low, extent, extent);
    const Image nearest = draw_textured(face, false, TextureFilter::Nearest);
    const Image sharp = draw_textured(face, false, TextureFilter::Sharp);
    // A grey stripe inside the face and the black pixel either side of it.
    const auto grey_weight = [&](const Image &picture, int stripe) {
        const float start = low_x + stripe_width * static_cast<float>(2 * stripe + 1);
        float sum = 0;
        for (int x = static_cast<int>(start) - 1; x <= static_cast<int>(start + stripe_width) + 1; ++x)
            sum += static_cast<float>(pixel(picture, x, row).r) / static_cast<float>(checker_grey.r);
        return sum;
    };
    float nearest_low = 1e9f, nearest_high = 0;
    for (int stripe = 0; stripe < 3; ++stripe) {
        CAPTURE(stripe);
        nearest_low = std::min(nearest_low, grey_weight(nearest, stripe));
        nearest_high = std::max(nearest_high, grey_weight(nearest, stripe));
        CHECK(std::abs(grey_weight(sharp, stripe) - stripe_width) <= 0.2f);
    }
    CHECK(nearest_high - nearest_low >= 1.0f);
    // Away from a texel's edge, sharp is the texel itself.
    for (int x = static_cast<int>(low_x) + 2; x < static_cast<int>(low_x + extent) - 1; ++x) {
        CAPTURE(x);
        const bool edge = pixel(nearest, x - 1, row) != pixel(nearest, x, row) ||
                          pixel(nearest, x + 1, row) != pixel(nearest, x, row);
        if (!edge)
            CHECK(pixel(sharp, x, row) == pixel(nearest, x, row));
    }
}

TEST_CASE("vulkan: past the enhanced depth the game's look is drawn") {
    // The wall at 14000: fogged per pixel nearer than that, the game's (black) corners past it.
    const Image near = draw_fogged(cued_wall(8), FogMode::Distance, wall_depth + 1000);
    CHECK(std::abs(pixel(near, 160, 120).r - game_cued(wall_depth, 2)) <= 1);
    const Image far = draw_fogged(cued_wall(8), FogMode::Distance, wall_depth - 1000);
    CHECK(pixel(far, 160, 120) == Rgb5{0, 0, 0});
    // The checker at 200: averaged nearer than that, its own texels past it.
    const std::vector<GpuVertex> checker = minified_square(checker_low);
    const Image filtered = draw_textured(checker, true, TextureFilter::Bilinear, 1, 300);
    CHECK(widest_step(pixel(filtered, 158, 118), Rgb5{15, 15, 15}) <= 1);
    const Image plain = draw_textured(checker, true, TextureFilter::Bilinear, 1, 100);
    const Rgb5 texel = pixel(plain, 158, 118);
    CHECK((texel == checker_grey || texel == Rgb5{0, 0, 0}));
}

namespace {
// Presents the uploaded frame through motions, each vertex carrying this much of its way back.
Image present_moved(const std::vector<Motion> &motions, float carry) {
    const FrameView view{motions, carry, true, false, 200, 160, 120, {1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, 0}};
    backend->present(view, {}, 0, 0, nullptr);
    std::optional<Image> picture = backend->capture();
    if (!picture) {
        FAIL("nothing captured");
        return {};
    }
    return std::move(*picture);
}

const Motion unmoved{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}};
constexpr std::array<u8, 4> white = {255, 255, 255, 255};

// A model part's 40 x 40 square about its origin, each corner's way back +40 in model x.
std::vector<GpuVertex> part_square() {
    std::vector<GpuVertex> vertices;
    add_rect(vertices, 0, 0, 1, 1, VertexTexturing::Solid, white);
    const std::array<std::array<float, 2>, 6> corners = {{{-20, -20}, {20, -20}, {-20, 20}, {20, -20}, {20, 20}, {-20, 20}}};
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        GpuVertex &vertex = vertices[i];
        vertex.w = vertex.cz = 200;
        vertex.lx = corners[i][0];
        vertex.ly = corners[i][1];
        vertex.bx = 40;
        vertex.part = 1;
    }
    return vertices;
}
} // namespace

TEST_CASE("vulkan: a model part is drawn where its motion and its way back put it") {
    const std::vector<GpuVertex> vertices = part_square();
    const std::vector<DrawRun> runs = {run_of(BlendMode::opaque, 0, vertices.size())};
    backend->upload_frame(vertices, runs, cleared_to(background), {});
    // Model space to camera: 200 ahead, where a unit is a pixel.
    const std::vector<Motion> motions = {unmoved, Motion{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 200}}};
    const Image drawn = present_moved(motions, 0);
    CHECK(pixel(drawn, 150, 120) == Rgb5{31, 31, 31});
    CHECK(pixel(drawn, 190, 120) == background);
    const Image halfway = present_moved(motions, 0.5f);
    CHECK(pixel(halfway, 150, 120) == background);
    CHECK(pixel(halfway, 190, 120) == Rgb5{31, 31, 31});
}

TEST_CASE("vulkan: camera-fixed faces move along their way back on screen, the dial about its pivot") {
    std::vector<GpuVertex> vertices;
    add_rect(vertices, 100, 40, 40, 20, VertexTexturing::Solid, white);   // a HUD bar
    for (GpuVertex &vertex : vertices)
        vertex.bx = 40;
    const std::size_t bar = vertices.size();
    add_rect(vertices, 170, 115, 20, 10, VertexTexturing::Solid, white);   // the needle
    for (std::size_t i = bar; i < vertices.size(); ++i)
        vertices[i].part = 1;
    const std::vector<DrawRun> runs = {run_of(BlendMode::opaque, 0, vertices.size())};
    backend->upload_frame(vertices, runs, cleared_to(background), {});
    // Half a turn about (160, 120).
    const std::vector<Motion> motions = {unmoved, Motion{{-1, 0, 0, 320, 0, -1, 0, 240, 0, 0, 1, 0}}};
    const Image shown = present_moved(motions, 0.5f);
    CHECK(pixel(shown, 110, 50) == background);
    CHECK(pixel(shown, 150, 50) == Rgb5{31, 31, 31});
    CHECK(pixel(shown, 180, 120) == background);
    CHECK(pixel(shown, 140, 120) == Rgb5{31, 31, 31});
}

TEST_CASE("vulkan: a floor reaching behind the camera is clipped at the shown camera's near plane") {
    // A floor 50 below the camera, from 1000 behind it to 2000 ahead: below
    // the horizon (y = 120 + 50 * 200 / 2000 = 125) it fills the picture.
    std::vector<GpuVertex> vertices;
    add_rect(vertices, 0, 0, 1, 1, VertexTexturing::Solid, white);
    const std::array<std::array<float, 2>, 6> corners = {{{-5000, -1000}, {5000, -1000}, {-5000, 2000},
                                                          {5000, -1000}, {5000, 2000}, {-5000, 2000}}};
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        GpuVertex &vertex = vertices[i];
        vertex.cx = corners[i][0];
        vertex.cy = 50;
        vertex.cz = corners[i][1];
        vertex.w = std::max(vertex.cz, 16.0f);
        vertex.x = 2000;   // as the game saturated it
        vertex.y = 2000;
    }
    const std::vector<DrawRun> runs = {run_of(BlendMode::opaque, 0, vertices.size())};
    backend->upload_frame(vertices, runs, cleared_to(background), {});
    const Image shown = present_moved({unmoved}, 0);
    CHECK(pixel(shown, 10, 239) == Rgb5{31, 31, 31});
    CHECK(pixel(shown, 160, 130) == Rgb5{31, 31, 31});
    CHECK(pixel(shown, 310, 239) == Rgb5{31, 31, 31});
    CHECK(pixel(shown, 160, 120) == background);
    CHECK(pixel(shown, 10, 10) == background);
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
    backend->upload_layer(mip_layer, mip_test_page());
    doctest::Context context(argc, argv);
    const int result = context.run();
    started.backend.reset();
    return result;
}
