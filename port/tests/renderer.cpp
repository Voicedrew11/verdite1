#include "../renderer/frame_batch.h"
#include "../renderer/texture_layers.h"
#include "../renderer/texture_mips.h"
#include "../renderer/model_transform.h"
#include "../renderer/vertex_carry.h"
#include "../renderer/view_rotation.h"
#include "../renderer/view_smoothing.h"

#include <kf/renderer/projection.h>

#include <kf/lib/fixed_math.h>

#include <doctest/doctest.h>

#include <cmath>
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

namespace {
Image blank_page() {
    return {texture_page_extent, texture_page_extent,
            ByteBuffer(std::size_t{texture_page_extent} * texture_page_extent * 4, 0)};
}

u8 *texel(Image &image, u32 x, u32 y) { return &image.rgba[(std::size_t{y} * image.width + x) * 4]; }
} // namespace

TEST_CASE("mips: every level down to one texel, STP next to opaque at level 0") {
    Image page = blank_page();
    texel(page, 0, 0)[3] = texture_alpha_opaque_marker;
    texel(page, 1, 0)[3] = texture_alpha_semitransparent_marker;
    std::vector<Image> levels = texture_mip_chain(page);
    REQUIRE(levels.size() == 9);
    CHECK(levels.back().width == 1);
    CHECK(levels.back().height == 1);
    CHECK(texel(levels[0], 0, 0)[3] == 255);
    CHECK(texel(levels[0], 1, 0)[3] == texture_mip_stp_alpha);
    CHECK(texel(levels[0], 2, 0)[3] == 0);
    CHECK(texture_mip_chain(Image{16, 16, ByteBuffer(std::size_t{16} * 16 * 4, 0)}).empty());
}

TEST_CASE("mips: a texel half covered keeps its colour over its coverage") {
    Image page = blank_page();
    for (const u32 x : {0u, 1u}) {
        u8 *covered = texel(page, x, 0);
        covered[0] = 200;
        covered[3] = texture_alpha_opaque_marker;
    }
    std::vector<Image> levels = texture_mip_chain(page);
    REQUIRE(levels.size() > 1);
    const u8 *below = texel(levels[1], 0, 0);
    CHECK(below[3] == 128);
    CHECK(below[0] * 255 / below[3] == doctest::Approx(200).epsilon(0.01));
}

TEST_CASE("transform: the exact point keeps the fraction the fixed-point rotation drops") {
    // A HUD model turned by 30 degrees about z: its corners land between pixels.
    MATRIX model{};
    model.m[0][0] = model.m[1][1] = 3547;
    model.m[0][1] = -2048;
    model.m[1][0] = 2048;
    model.m[2][2] = 4096;
    model.t[0] = 40;
    model.t[1] = 200;
    const SVECTOR point{7, 3, 0};
    const VECTOR whole = render_transform_point(model, point);
    const auto exact = render_transform_point_exact(model, point);
    CHECK(exact[0] == doctest::Approx(40 + (3547.0 * 7 - 2048.0 * 3) / 4096));
    CHECK(exact[1] == doctest::Approx(200 + (2048.0 * 7 + 3547.0 * 3) / 4096));
    CHECK(std::abs(exact[0] - whole.vx) < 1);
    CHECK(std::abs(exact[1] - whole.vy) < 1);
    CHECK(exact[0] != whole.vx);
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

TEST_CASE("batch: affine faces crossing the near plane are cut and re-projected") {
    TextureLayers layers;
    DrawFace face = solid_face(0, 100);
    face.vertices[2].cz = 4;   // behind the near plane
    for (Vertex &vertex : face.vertices)
        vertex.w = 0;          // affine: the GPU cannot clip it
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

TEST_CASE("batch: perspective faces reaching behind the camera are left whole for the GPU") {
    // Cut at the update's camera, the part behind it stayed missing while
    // smoothing turned the shown view toward it.
    TextureLayers layers;
    DrawFace face = solid_face(0, 100, FaceShape::Quad);
    face.vertices[2].cz = -500;
    face.vertices[3].cz = -500;
    face.vertices[2].x = 2000;   // saturated as the game projected it
    BatchRules rules;
    rules.near_clip = Projection{};
    rules.depth_buffer = true;
    const std::vector<DrawFace> faces = {face};
    const FrameBatch batch = batch_faces(faces, rules, layers);
    REQUIRE(batch.vertices.size() == 6);
    CHECK(batch.vertices[2].cz == -500);
    REQUIRE(batch.runs.size() == 1);
    CHECK(batch.runs[0].state.depth_test);
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

TEST_CASE("batch: a face keeps its part, and a part the update does not have is none") {
    TextureLayers layers;
    std::vector<DrawFace> faces = {solid_face(0), solid_face(1)};
    faces[0].part = 2;
    faces[0].vertices[0].bx = 5;
    faces[1].part = 3;
    BatchRules rules;
    rules.part_count = 2;
    const FrameBatch batch = batch_faces(faces, rules, layers);
    CHECK(batch.vertices[0].part == 0);   // faces[1], deeper, drawn first: part 3 of 2
    CHECK(batch.vertices[3].part == 2);
    CHECK(batch.vertices[3].bx == 5);
}

TEST_CASE("batch: a cut face's new corners carry their share of the way back") {
    TextureLayers layers;
    DrawFace face = solid_face(0, 100);
    face.vertices[0].cz = 10;   // behind the near plane (32)
    face.vertices[0].bx = 0;
    face.vertices[1].bx = 90;
    face.vertices[2].bx = 90;
    BatchRules rules;
    rules.near_clip = Projection{};
    const FrameBatch batch = batch_faces({&face, 1}, rules, layers);
    REQUIRE_FALSE(batch.vertices.empty());
    for (const GpuVertex &vertex : batch.vertices)
        CHECK(vertex.bx == doctest::Approx(90 * (vertex.cz - 10) / 90).epsilon(1e-3));
}

TEST_CASE("batch: a cued face carries its clear colour and cue scale, a cut corner its share") {
    TextureLayers layers;
    DrawFace face = solid_face(0, 100);
    face.depth_cue_scale = 1.5f;
    face.vertices[0].cz = 10;   // behind the near plane (32)
    face.vertices[0].clear_r = 0;
    face.vertices[1].clear_r = 90.0f / 255;
    face.vertices[2].clear_r = 90.0f / 255;
    BatchRules rules;
    rules.near_clip = Projection{};
    const FrameBatch batch = batch_faces({&face, 1}, rules, layers);
    REQUIRE_FALSE(batch.vertices.empty());
    for (const GpuVertex &vertex : batch.vertices) {
        CHECK(vertex.cue_scale_quarters == 6);
        CHECK(std::abs(vertex.clear_color[0] - (vertex.cz - 10)) <= 1.0f);   // 90 over the 90 units from z 10 to 100
    }
}

namespace {
// Views up, down and at odd angles, where the game's 12-bit matrix is furthest off.
const std::vector<SVECTOR> odd_views = {{0, 0, 0}, {-300, 700, 0}, {150, 1365, 0}, {-100, 3001, 0}, {200, 2222, 41}};
} // namespace

TEST_CASE("view rotation: the game's matrix is undone exactly, and the exact one is a rotation near it") {
    for (const SVECTOR &angles : odd_views) {
        MATRIX fixed{};
        matrix_set_rotation_xyz(angles, fixed);
        const Rotation3 inverse = game_view_inverse(angles);
        const Rotation3 exact = exact_view_rotation(angles);
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                double undone = 0, square = 0;
                for (int k = 0; k < 3; ++k) {
                    undone += inverse[r][k] * fixed.m[k][c] / 4096.0;
                    square += exact[r][k] * exact[c][k];
                }
                CHECK(undone == doctest::Approx(r == c ? 1 : 0).epsilon(1e-12));
                CHECK(square == doctest::Approx(r == c ? 1 : 0).epsilon(1e-12));
                // The game's truncating shifts add up to a few 4096ths; a different order would be far off.
                CHECK(std::abs(exact[r][c] - fixed.m[r][c] / 4096.0) < 8 / 4096.0);
            }
        }
    }
}

TEST_CASE("smoothing: a still view moves what the game projected to where the exact rotation puts it") {
    for (const SVECTOR &angles : odd_views) {
        CameraPose camera = pose_at(0);
        camera.rotation = angles;
        ViewSmoothing smoothing;
        smoothing.take_update(camera, {0, 0, 0}, {}, 0);
        const ShownView view = smoothing.view_exact();
        REQUIRE(view.reproject);
        CHECK(view.carry == 0);
        MATRIX fixed{};
        matrix_set_rotation_xyz(angles, fixed);
        const Rotation3 exact = exact_view_rotation(angles);
        const std::array<double, 3> world = {-800, 200, 1500};
        const auto &m = view.motions[0].to_shown;
        for (int r = 0; r < 3; ++r) {
            double shown = m[r * 4 + 3], wanted = 0;
            for (int k = 0; k < 3; ++k) {
                double game = 0;
                for (int j = 0; j < 3; ++j)
                    game += fixed.m[k][j] / 4096.0 * world[j];
                shown += m[r * 4 + k] * game;
                wanted += exact[r][k] * world[k];
            }
            CHECK(shown == doctest::Approx(wanted).epsilon(1e-5));
        }
    }
}

TEST_CASE("smoothing: the first update is shown as drawn") {
    ViewSmoothing smoothing;
    smoothing.take_update(pose_at(0), {0, 0, 0}, {}, 0);
    const ShownView view = smoothing.view_at(25 * ms, {0, 0, 0});
    REQUIRE(view.motions.size() == 1);
    CHECK(view.motions[0].to_shown[3] == doctest::Approx(0));
    CHECK(view.reproject);
}

TEST_CASE("smoothing: halfway between two updates") {
    ViewSmoothing smoothing;
    smoothing.take_update(pose_at(0), {0, 0, 0}, {}, 0);
    smoothing.take_update(pose_at(100), {0, 0, 0}, {}, 50 * ms);
    const ShownView view = smoothing.view_at(75 * ms, {0, 0, 0});
    // The shown camera trails the drawn one by 50: drawn points move +50 in x.
    CHECK(view.motions[0].to_shown[3] == doctest::Approx(50));
    CHECK(view.camera_world[0] == doctest::Approx(50));
    const ShownView done = smoothing.view_at(100 * ms, {0, 0, 0});
    CHECK(done.motions[0].to_shown[3] == doctest::Approx(0));
}

TEST_CASE("smoothing: look input shows without an update of lag") {
    ViewSmoothing smoothing;
    smoothing.take_update(pose_at(0, 0), {0, 0, 0}, {}, 0);
    smoothing.take_update(pose_at(0, 200), {0, 200, 0}, {}, 50 * ms);
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
    smoothing.take_update(pose_at(0), {0, 0, 0}, {}, 0);
    smoothing.take_update(pose_at(9000), {0, 0, 0}, {}, 50 * ms);
    CHECK(smoothing.view_at(50 * ms, {0, 0, 0}).motions[0].to_shown[3] == doctest::Approx(0));
}

namespace {
std::array<float, 12> placed_at(float x, float yaw_radians = 0) {
    const float c = std::cos(yaw_radians), s = std::sin(yaw_radians);
    return {c, 0, -s, x, 0, 1, 0, 0, s, 0, c, 2000};
}

float column_length(const Motion &motion, int c) {
    const auto &m = motion.to_shown;
    return std::sqrt(m[c] * m[c] + m[4 + c] * m[4 + c] + m[8 + c] * m[8 + c]);
}
} // namespace

TEST_CASE("smoothing: a moving model is shown between its last two places") {
    ViewSmoothing smoothing;
    const std::vector<PartMove> parts = {ModelMove{placed_at(1000), placed_at(1100)}};
    smoothing.take_update(pose_at(0), {0, 0, 0}, {}, 0);
    smoothing.take_update(pose_at(0), {0, 0, 0}, parts, 50 * ms);
    const ShownView view = smoothing.view_at(75 * ms, {0, 0, 0});
    REQUIRE(view.motions.size() == 2);
    CHECK(view.carry == doctest::Approx(0.5));
    CHECK(view.motions[1].to_shown[3] == doctest::Approx(1050));
    CHECK(view.motions[1].to_shown[11] == doctest::Approx(2000));
    const ShownView done = smoothing.view_at(100 * ms, {0, 0, 0});
    CHECK(done.carry == 0);
    CHECK(done.motions[1].to_shown[3] == doctest::Approx(1100));
}

TEST_CASE("smoothing: a turning model keeps its size halfway") {
    ViewSmoothing smoothing;
    const std::vector<PartMove> parts = {ModelMove{placed_at(0, 0), placed_at(0, 3.14159265f / 2)}};
    smoothing.take_update(pose_at(0), {0, 0, 0}, {}, 0);
    smoothing.take_update(pose_at(0), {0, 0, 0}, parts, 50 * ms);
    const Motion halfway = smoothing.view_at(75 * ms, {0, 0, 0}).motions[1];
    CHECK(column_length(halfway, 0) == doctest::Approx(1));
    CHECK(halfway.to_shown[0] == doctest::Approx(0.7071).epsilon(1e-3));
}

TEST_CASE("smoothing: a warp shows the update's own places, with no way back") {
    ViewSmoothing smoothing;
    const std::vector<PartMove> parts = {ModelMove{placed_at(1000), placed_at(1100)}};
    smoothing.take_update(pose_at(0), {0, 0, 0}, {}, 0);
    smoothing.take_update(pose_at(9000), {0, 0, 0}, parts, 50 * ms);
    const ShownView view = smoothing.view_at(60 * ms, {0, 0, 0});
    CHECK(view.carry == 0);
    CHECK(view.motions[1].to_shown[3] == doctest::Approx(1100 - 9000));
}

TEST_CASE("smoothing: the compass needle turns with the view shown") {
    ViewSmoothing smoothing;
    const std::vector<PartMove> parts = {DialTurn{286, 34}};
    smoothing.take_update(pose_at(0), {0, 0, 0}, {}, 0);
    smoothing.take_update(pose_at(0), {0, 0, 0}, parts, 50 * ms);
    // Pending look turns the view 512 (an eighth of a turn); the needle's angle is the negated yaw.
    const Motion dial = smoothing.view_at(100 * ms, {0, 512, 0}).motions[1];
    CHECK(dial.to_shown[0] == doctest::Approx(0.7071).epsilon(1e-3));
    CHECK(dial.to_shown[4] == doctest::Approx(-0.7071).epsilon(1e-3));
    // The pivot stays put.
    CHECK(dial.to_shown[0] * 286 + dial.to_shown[1] * 34 + dial.to_shown[3] == doctest::Approx(286));
    CHECK(dial.to_shown[4] * 286 + dial.to_shown[5] * 34 + dial.to_shown[7] == doctest::Approx(34));
}

namespace {
// A model at world position x, y, z, unturned, as the game hands it seen from camera.
MATRIX seen_model(const CameraPose &camera, s32 x, s32 y, s32 z) {
    MATRIX view{};
    matrix_set_rotation_xyz(camera.rotation, view);
    MATRIX out = view;
    const std::array<s32, 3> relative = {x - camera.position.vx, y - camera.position.vy, z - camera.position.vz};
    for (int r = 0; r < 3; ++r)
        out.t[r] = (view.m[r][0] * relative[0] + view.m[r][1] * relative[1] + view.m[r][2] * relative[2]) / 4096;
    return out;
}

constexpr PartOwner actor{256, 3};
} // namespace

TEST_CASE("carry: a model drawn again carries its place and each vertex's way back") {
    VertexCarry carry;
    const CameraPose camera = pose_at(0);
    const std::vector<SVECTOR> before = {{0, 0, 0}, {10, 0, 0}}, after = {{0, 0, 0}, {12, -4, 0}};
    carry.begin_update();
    carry.seen_from(camera);
    CHECK(carry.carry_model(actor, seen_model(camera, 1000, 0, 2000), before).part == 0);
    carry.begin_update();
    carry.seen_from(camera);
    const CarriedModel carried = carry.carry_model(actor, seen_model(camera, 1100, 0, 2000), after);
    REQUIRE(carried.part == 1);
    REQUIRE(carried.back.size() == 2);
    CHECK(carried.back[1][0] == -2);
    CHECK(carried.back[1][1] == 4);
    REQUIRE(carry.parts().size() == 1);
    const auto &move = std::get<ModelMove>(carry.parts()[0]);
    CHECK(move.before[3] == doctest::Approx(1000));
    CHECK(move.now[3] == doctest::Approx(1100));
    CHECK(move.now[11] == doctest::Approx(2000));
}

TEST_CASE("carry: a model's place is in the world, whichever way the camera turned") {
    VertexCarry carry;
    const CameraPose facing = pose_at(500, 0), turned = pose_at(500, 1024);
    const std::vector<SVECTOR> vertices = {{0, 0, 0}};
    carry.begin_update();
    carry.seen_from(facing);
    (void)carry.carry_model(actor, seen_model(facing, 1000, 0, 2000), vertices);
    carry.begin_update();
    carry.seen_from(turned);
    REQUIRE(carry.carry_model(actor, seen_model(turned, 1000, 0, 2000), vertices).part == 1);
    const auto &move = std::get<ModelMove>(carry.parts()[0]);
    for (const int i : {0, 3, 5, 10, 11})
        CHECK(move.now[i] == doctest::Approx(move.before[i]).epsilon(1e-2));
    CHECK(move.now[3] == doctest::Approx(1000).epsilon(1e-2));
}

TEST_CASE("carry: a new occupant, a warp or a nameless model carries nothing") {
    VertexCarry carry;
    const CameraPose camera = pose_at(0);
    const std::vector<SVECTOR> vertices = {{0, 0, 0}};
    carry.begin_update();
    carry.seen_from(camera);
    (void)carry.carry_model(actor, seen_model(camera, 0, 0, 2000), vertices);
    (void)carry.carry_model({257, 3}, seen_model(camera, 0, 0, 2000), vertices);
    (void)carry.carry_model({0, 0}, seen_model(camera, 0, 0, 2000), vertices);
    carry.begin_update();
    carry.seen_from(camera);
    CHECK(carry.carry_model({256, 4}, seen_model(camera, 0, 0, 2000), vertices).part == 0);
    CHECK(carry.carry_model({257, 3}, seen_model(camera, 2500, 0, 2000), vertices).part == 0);
    CHECK(carry.carry_model({0, 0}, seen_model(camera, 0, 0, 2000), vertices).part == 0);
    CHECK(carry.parts().empty());
}

TEST_CASE("carry: drawings under one tag are matched in order, past a 2D update") {
    VertexCarry carry;
    const CameraPose camera = pose_at(0);
    const std::vector<SVECTOR> one = {{0, 0, 0}}, two = {{0, 0, 0}, {1, 0, 0}};
    carry.begin_update();
    carry.seen_from(camera);
    (void)carry.carry_model(actor, seen_model(camera, 0, 0, 2000), one);
    (void)carry.carry_model(actor, seen_model(camera, 0, 0, 2000), two);
    carry.begin_update();   // a menu: nothing carried, nothing lost
    CHECK(carry.carry_model(actor, seen_model(camera, 0, 0, 2000), one).part == 0);
    carry.begin_update();
    carry.seen_from(camera);
    CHECK(carry.carry_model(actor, seen_model(camera, 0, 0, 2000), one).part == 1);
    CHECK(carry.carry_model(actor, seen_model(camera, 0, 0, 2000), two).part == 2);
}

TEST_CASE("carry: screen points carry their way back") {
    VertexCarry carry;
    const std::vector<std::array<float, 2>> before = {{10, 20}, {60, 20}}, after = {{10, 20}, {50, 20}};
    carry.begin_update();
    carry.seen_from(pose_at(0));
    CHECK(carry.carry_screen({1312, 0}, before).empty());
    carry.begin_update();
    carry.seen_from(pose_at(0));
    const auto back = carry.carry_screen({1312, 0}, after);
    REQUIRE(back.size() == 2);
    CHECK(back[0][0] == 0);
    CHECK(back[1][0] == 10);
    CHECK(carry.add_dial(286, 34) == 1);
}

TEST_CASE("model transform: an axis scaled from nothing grows without turning the rest") {
    const std::array<double, 12> flat = {1, 0, 0, 0, 0, 0, 0, 5, 0, 0, 1, 0};   // y scaled to 0
    const std::array<double, 12> tall = {1, 0, 0, 0, 0, 2, 0, 5, 0, 0, 1, 0};
    const auto halfway = blend_transforms(split_transform(flat), split_transform(tall), 0.5);
    CHECK(halfway[0] == doctest::Approx(1));
    CHECK(halfway[5] == doctest::Approx(1));
    CHECK(halfway[10] == doctest::Approx(1));
    CHECK(halfway[7] == doctest::Approx(5));
}

TEST_CASE("model transform: the turn between two rotations") {
    const std::array<double, 12> facing = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    const std::array<double, 12> turned = {0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, 0};
    CHECK(turn_between(split_transform(facing), split_transform(turned)) == doctest::Approx(3.14159265 / 2));
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
