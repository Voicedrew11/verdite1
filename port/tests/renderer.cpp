#include "../renderer/frame_batch.h"
#include "../renderer/texture_layers.h"
#include "../renderer/view_smoothing.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

using namespace kf;

namespace {
TextureSource page(u16 x, u16 palette_x = 0) { return {x, 0, palette_x, 256, TextureFormat::Indexed4}; }

Vertex corner(float x, float y, float cz = 0) {
    Vertex v{};
    v.x = x;
    v.y = y;
    v.r = v.g = v.b = v.a = 1;
    if (cz > 0) {
        v.w = cz;
        // Projected at the default distance (200) and centre (160, 120).
        v.cx = (x - 160) * cz / 200;
        v.cy = (y - 120) * cz / 200;
        v.cz = cz;
    }
    return v;
}

DrawFace solid_face(s32 depth, float cz = 0, FaceShape shape = FaceShape::Triangle) {
    DrawFace face{};
    face.vertices = {corner(0, 0, cz), corner(10, 0, cz), corner(0, 10, cz), corner(10, 10, cz)};
    face.shape = shape;
    face.material = {SurfaceKind::Solid, {}, BlendMode::opaque};
    face.transparency = FaceTransparency::Opaque;
    face.depth = depth;
    return face;
}

DrawFace textured_face(s32 depth, BlendMode blend, FaceTransparency transparency) {
    DrawFace face = solid_face(depth);
    face.material = {SurfaceKind::Texture, page(0), blend};
    face.transparency = transparency;
    return face;
}

CameraPose pose_at(s32 x, s16 yaw = 0) {
    CameraPose pose{};
    pose.position = {x, 0, 0};
    pose.rotation = {0, yaw, 0};
    return pose;
}

constexpr std::uint64_t ms = 1'000'000;
} // namespace

TEST_CASE("texture layers: one layer per page and palette") {
    TextureLayers layers;
    layers.begin_update();
    const auto first = layers.layer_for(page(0));
    REQUIRE(first);
    CHECK(layers.layer_for(page(0)) == first);
    CHECK(layers.layer_for(page(0, 16)) != first);
    CHECK(layers.take_uploads().size() == 2);
    CHECK(layers.take_uploads().empty());
}

TEST_CASE("texture layers: direct colour ignores the palette") {
    TextureLayers layers;
    layers.begin_update();
    const auto a = layers.layer_for({64, 0, 0, 0, TextureFormat::Direct16});
    const auto b = layers.layer_for({64, 0, 32, 480, TextureFormat::Direct16});
    CHECK(a == b);
}

TEST_CASE("texture layers: full within one update, reused across updates") {
    TextureLayers layers;
    layers.begin_update();
    for (u16 i = 0; i < texture_layer_count; ++i)
        REQUIRE(layers.layer_for(page(0, static_cast<u16>(i * 16))));
    CHECK_FALSE(layers.layer_for(page(64)));
    layers.take_uploads();

    layers.begin_update();
    const auto kept = layers.layer_for(page(0, 0));
    const auto reused = layers.layer_for(page(64));
    REQUIRE(reused);
    CHECK(reused != kept);
    const auto uploads = layers.take_uploads();
    REQUIRE(uploads.size() == 1);
    CHECK(reused == uploads[0].layer);
}

TEST_CASE("texture layers: invalidate uploads again") {
    TextureLayers layers;
    layers.begin_update();
    layers.layer_for(page(0));
    layers.take_uploads();
    layers.invalidate();
    layers.begin_update();
    layers.layer_for(page(0));
    CHECK(layers.take_uploads().size() == 1);
}

TEST_CASE("batch: farthest first, later submissions first among equals") {
    TextureLayers layers;
    std::vector<DrawFace> faces = {solid_face(10), solid_face(30), solid_face(10)};
    faces[0].vertices[0].r = 0.1f;
    faces[2].vertices[0].r = 0.2f;
    const FrameBatch batch = batch_faces(faces, {}, layers);
    REQUIRE(batch.vertices.size() == 9);
    CHECK(batch.vertices[3].color[0] == 51);   // face 2 (0.2 * 255) before face 0
    CHECK(batch.vertices[6].color[0] == 26);
}

TEST_CASE("batch: quads are (0,1,2) and (1,3,2), one run") {
    TextureLayers layers;
    const std::vector<DrawFace> faces = {solid_face(0, 0, FaceShape::Quad)};
    const FrameBatch batch = batch_faces(faces, {}, layers);
    REQUIRE(batch.vertices.size() == 6);
    CHECK(batch.vertices[3].x == 10);
    CHECK(batch.vertices[4].x == 10);
    CHECK(batch.vertices[4].y == 10);
    REQUIRE(batch.runs.size() == 1);
    CHECK(batch.runs[0].vertex_count == 6);
}

TEST_CASE("batch: an untextured translucent face blends as the last textured face set") {
    TextureLayers layers;
    layers.begin_update();
    DrawFace near = solid_face(5);
    near.transparency = FaceTransparency::Blend;
    DrawFace far = near;
    far.depth = 20;
    // Drawn far, textured, near: the initial blend, opaque, then the textured face's add.
    const std::vector<DrawFace> faces = {far, textured_face(10, BlendMode::add, FaceTransparency::Opaque), near};
    BatchRules rules;
    rules.initial_blend = BlendMode::subtract;
    const FrameBatch batch = batch_faces(faces, rules, layers);
    REQUIRE(batch.runs.size() == 3);
    CHECK(batch.runs[0].state.blend == BlendMode::subtract);
    CHECK(batch.runs[1].state.blend == BlendMode::opaque);
    CHECK(batch.runs[2].state.blend == BlendMode::add);
}

TEST_CASE("batch: dither and colour scale follow the PS1") {
    TextureLayers layers;
    layers.begin_update();
    DrawFace gouraud = solid_face(0);
    gouraud.shading = FaceShading::Gouraud;
    const std::vector<DrawFace> faces = {gouraud, solid_face(1),
                                         textured_face(2, BlendMode::average, FaceTransparency::Opaque)};
    const FrameBatch batch = batch_faces(faces, {}, layers);
    REQUIRE(batch.vertices.size() == 9);
    CHECK(batch.vertices[0].texturing == VertexTexturing::Modulated);   // textured, drawn first
    CHECK(batch.vertices[0].color[0] == 128);                           // 1.0 is unity: 128
    CHECK(batch.vertices[0].dither == 1);
    CHECK(batch.vertices[3].dither == 0);                               // flat solid
    CHECK(batch.vertices[3].color[0] == 255);
    CHECK(batch.vertices[6].dither == 1);                               // gouraud solid
}

TEST_CASE("batch: faces crossing the near plane are cut and re-projected") {
    TextureLayers layers;
    DrawFace face = solid_face(0, 100);
    face.vertices[2].cz = 4;   // behind the near plane
    face.vertices[2].w = 4;
    BatchRules rules;
    rules.near_clip = Projection{};
    const std::vector<DrawFace> faces = {face};
    const FrameBatch batch = batch_faces(faces, rules, layers);
    REQUIRE(batch.vertices.size() == 6);   // the cut quadrilateral, fanned
    for (const GpuVertex &v : batch.vertices) {
        CHECK(v.cz >= render_near_depth);
        CHECK(v.x == doctest::Approx(v.cx * 200 / v.cz + 160));
    }
}

TEST_CASE("batch: oversized 2D triangles are rejected, world ones kept") {
    TextureLayers layers;
    DrawFace flat = solid_face(0);
    flat.vertices[1].x = 2000;
    DrawFace world = solid_face(1, 100);
    world.vertices[1].x = 2000;
    const std::vector<DrawFace> faces = {flat, world};
    const FrameBatch batch = batch_faces(faces, {}, layers);
    CHECK(batch.vertices.size() == 3);
    CHECK(batch.vertices[0].cz == 100);
}

TEST_CASE("batch: depth test only for world faces with the depth buffer on") {
    TextureLayers layers;
    const std::vector<DrawFace> faces = {solid_face(0, 100), solid_face(1)};
    BatchRules rules;
    rules.depth_buffer = true;
    const FrameBatch batch = batch_faces(faces, rules, layers);
    REQUIRE(batch.runs.size() == 2);
    CHECK_FALSE(batch.runs[0].state.depth_test);   // the 2D face, deeper, drawn first
    CHECK(batch.runs[1].state.depth_test);
    CHECK(batch.runs[1].state.depth_write);
}

TEST_CASE("batch: entities are numbered from 1 in draw order") {
    TextureLayers layers;
    std::vector<DrawFace> faces = {solid_face(0), solid_face(1), solid_face(2)};
    faces[0].entity = 7;
    faces[1].entity = 0;
    faces[2].entity = 900;
    const FrameBatch batch = batch_faces(faces, {}, layers);
    CHECK(batch.entity_tags == std::vector<u16>{900, 7});
    CHECK(batch.vertices[0].entity == 1);
    CHECK(batch.vertices[3].entity == 0);
    CHECK(batch.vertices[6].entity == 2);
}

TEST_CASE("smoothing: the first update is shown as drawn") {
    ViewSmoothing smoothing;
    smoothing.take_update(pose_at(0), {0, 0, 0}, {}, {}, 0);
    const ShownView view = smoothing.view_at(25 * ms, {0, 0, 0});
    REQUIRE(view.motions.size() == 1);
    CHECK(view.motions[0].to_shown[3] == doctest::Approx(0));
    CHECK(view.reproject);
}

TEST_CASE("smoothing: halfway between two updates") {
    ViewSmoothing smoothing;
    smoothing.take_update(pose_at(0), {0, 0, 0}, {}, {}, 0);
    smoothing.take_update(pose_at(100), {0, 0, 0}, {}, {}, 50 * ms);
    const ShownView view = smoothing.view_at(75 * ms, {0, 0, 0});
    // The shown camera trails the drawn one by 50: drawn points move +50 in x.
    CHECK(view.motions[0].to_shown[3] == doctest::Approx(50));
    CHECK(view.camera_world[0] == doctest::Approx(50));
    const ShownView done = smoothing.view_at(100 * ms, {0, 0, 0});
    CHECK(done.motions[0].to_shown[3] == doctest::Approx(0));
}

TEST_CASE("smoothing: look input shows without an update of lag") {
    ViewSmoothing smoothing;
    smoothing.take_update(pose_at(0, 0), {0, 0, 0}, {}, {}, 0);
    smoothing.take_update(pose_at(0, 200), {0, 200, 0}, {}, {}, 50 * ms);
    // All of the turn was look input: the shown view is the drawn one throughout.
    for (const std::uint64_t at : {50 * ms, 60 * ms, 99 * ms}) {
        const ShownView view = smoothing.view_at(at, {0, 0, 0});
        CHECK(view.motions[0].to_shown[0] == doctest::Approx(1).epsilon(1e-3));
        CHECK(view.motions[0].to_shown[2] == doctest::Approx(0).epsilon(1e-3));
    }
    // Pending look turns the view past the drawn update.
    const ShownView ahead = smoothing.view_at(100 * ms, {0, 512, 0});
    CHECK(ahead.motions[0].to_shown[0] == doctest::Approx(0.7071).epsilon(1e-2));
}

TEST_CASE("smoothing: a warp is not blended") {
    ViewSmoothing smoothing;
    smoothing.take_update(pose_at(0), {0, 0, 0}, {}, {}, 0);
    smoothing.take_update(pose_at(9000), {0, 0, 0}, {}, {}, 50 * ms);
    CHECK(smoothing.view_at(50 * ms, {0, 0, 0}).motions[0].to_shown[3] == doctest::Approx(0));
}

TEST_CASE("smoothing: a moving entity trails its last pose") {
    ViewSmoothing smoothing;
    const std::vector<u16> tags = {5};
    const std::vector<EntityPose> before = {{5, 1000, 0, 2000, 0, 0}};
    const std::vector<EntityPose> after = {{5, 1100, 0, 2000, 0, 0}};
    smoothing.take_update(pose_at(0), {0, 0, 0}, before, tags, 0);
    smoothing.take_update(pose_at(0), {0, 0, 0}, after, tags, 50 * ms);
    const ShownView view = smoothing.view_at(75 * ms, {0, 0, 0});
    REQUIRE(view.motions.size() == 2);
    CHECK(view.motions[0].to_shown[3] == doctest::Approx(0));
    CHECK(view.motions[1].to_shown[3] == doctest::Approx(-50));
}

TEST_CASE("smoothing: lights move with the camera, nearest first") {
    const std::vector<GameLight> lights = {{0, 0, 900, 500, 1, 1, 1, 0, 1}, {0, 0, 300, 500, 1, 0, 0, 0, 2}};
    Motion shift{};
    shift.to_shown = {1, 0, 0, 10, 0, 1, 0, 0, 0, 0, 1, 0};
    const auto shown = shown_lights(lights, 2.0f, shift, 0);
    REQUIRE(shown.size() == 2);
    CHECK(shown[0].z == 300);
    CHECK(shown[0].x == 10);
    CHECK(shown[0].r == 2);
    CHECK(shown[0].g == 0);
}
