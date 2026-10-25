#include <kf/renderer/backend.h>
#include <kf/renderer/frontend.h>

#include "frame_batch.h"
#include "light_sets.h"
#include "texture_layers.h"
#include "view_smoothing.h"

#include <cstdio>
#include <utility>
#include <vector>

namespace kf {
namespace {
constexpr int maximum_scale = 16;

struct Frontend {
    std::unique_ptr<RenderBackend> backend;
    PictureOptions options;            // as the settings asked; drawn_options() is what draws
    TextureStore textures;
    TextureLayers layers;
    std::uint64_t textures_seen = 0;   // TextureStore::generation the layers were decoded from
    ViewSmoothing smoothing;
    std::vector<GameLight> lights;
    std::vector<LightSet> light_sets;
    std::vector<u16> light_set_parts;   // light_set_parts of the last update
    int logical_width = render_width;
    int scale = 1;
};
Frontend frontend;

PictureOptions drawn_options() { return frontend.backend->drawable(frontend.options); }

// The depth cue of a world update; a 2D one has none to apply.
DepthCue depth_cue(const std::optional<CameraPose> &camera) {
    DepthCue cue;
    if (!camera)
        return cue;
    cue.world = true;
    cue.slope = static_cast<float>(render_fog_slope(camera->projection));
    cue.projection_distance = static_cast<float>(camera->projection.distance);
    const LightColor &fog = camera->fog_color;
    cue.far_color = {static_cast<float>(fog.r), static_cast<float>(fog.g), static_cast<float>(fog.b)};
    return cue;
}

// Sub-pixel pictures see the world through the exact view rotation; whole-pixel
// ones keep the game's own, as drawn, unless smoothing reprojects them anyway.
ShownView shown_view(const PictureOptions &options, std::uint64_t now_ns, const SVECTOR &pending_look) {
    if (options.smoothing)
        return frontend.smoothing.view_at(now_ns, pending_look);
    if (render_subpixel)
        return frontend.smoothing.view_exact();
    return frontend.smoothing.view_as_drawn();
}

void upload_new_layers() {
    for (const LayerUpload &upload : frontend.layers.take_uploads()) {
        Image texels{};
        const auto &words = frontend.textures.words;
        if (!texture_decode(&texels, upload.source, words.data(), words.size())) {
            std::fprintf(stderr, "[renderer] cannot decode texture page %u,%u\n", upload.source.x, upload.source.y);
            continue;
        }
        frontend.backend->upload_layer(upload.layer, texels);
    }
}
} // namespace

bool renderer_start(std::unique_ptr<RenderBackend> backend) {
    if (!backend)
        return false;
    frontend.backend = std::move(backend);
    frontend.layers.invalidate();
    frontend.backend->set_options(drawn_options());
    return frontend.backend->set_target(frontend.logical_width, frontend.scale);
}

void renderer_stop() { frontend.backend.reset(); }

Tier renderer_tier() { return frontend.backend ? frontend.backend->tier() : Tier::Classic; }

void renderer_set_options(const PictureOptions &options) {
    frontend.options = options;
    if (frontend.backend)
        frontend.backend->set_options(drawn_options());
}

bool renderer_set_target(int logical_width, int scale) {
    if (logical_width < render_width || scale < 1 || scale > maximum_scale)
        return false;
    logical_width += logical_width & 1;   // the original columns stay centred
    if (frontend.backend && !frontend.backend->set_target(logical_width, scale))
        return false;
    frontend.logical_width = logical_width;
    frontend.scale = scale;
    return true;
}

int renderer_extra_columns() { return (frontend.logical_width - render_width) / 2; }

PresentRect renderer_present_rect(int window_width, int window_height) {
    return letterbox(frontend.logical_width, window_width, window_height);
}

TextureStore &renderer_textures() { return frontend.textures; }

void renderer_set_occluders(std::span<const u8> solid, int columns, int rows, float tile_size) {
    if (frontend.backend)
        frontend.backend->set_occluders(solid, columns, rows, tile_size);
}

void renderer_submit(const GameFrame &frame, std::uint64_t at_ns) {
    if (!frontend.backend)
        return;
    if (frontend.textures.generation != frontend.textures_seen) {
        frontend.layers.invalidate();
        frontend.textures_seen = frontend.textures.generation;
    }
    const PictureOptions options = drawn_options();
    BatchRules rules;
    rules.initial_blend = frame.style.initial_blend;
    rules.depth_buffer = options.depth_buffer;
    if (frame.camera)
        rules.near_clip = frame.camera->projection;
    rules.subpixel = render_subpixel;
    rules.part_count = frame.parts.size();
    rules.light_set_count = options.pixel_lighting ? frame.light_sets.size() : 0;
    frontend.layers.begin_update();
    const FrameBatch batch = batch_faces(frame.faces, rules, frontend.layers);
    upload_new_layers();
    frontend.backend->upload_frame(batch.vertices, batch.runs, frame.style, depth_cue(frame.camera));
    frontend.smoothing.take_update(frame.camera, frame.look_applied, frame.parts, at_ns);
    frontend.lights.assign(frame.lights.begin(), frame.lights.end());
    frontend.light_sets.assign(frame.light_sets.begin(), frame.light_sets.begin() + rules.light_set_count);
    frontend.light_set_parts = light_set_parts(rules.light_set_count, frame.faces, frame.parts);
}

void renderer_present(std::uint64_t now_ns, const SVECTOR &pending_look, int window_width, int window_height,
                      ImDrawData *overlay) {
    if (!frontend.backend)
        return;
    const PictureOptions options = drawn_options();
    const ShownView shown = shown_view(options, now_ns, pending_look);
    std::vector<RenderLight> lights;
    if (options.dynamic_lights)
        lights = shown_lights(frontend.lights, options.light_strength, shown.motions.front(), now_ns);
    const std::span<const u16> parts = shown.reproject ? std::span<const u16>{frontend.light_set_parts} : std::span<const u16>{};
    const std::vector<ShownLightSet> light_sets = shown_light_sets(frontend.light_sets, parts, shown.motions);
    const FrameView view{shown.motions,  shown.carry,    shown.reproject,       !render_subpixel, shown.projection_distance,
                         shown.center_x, shown.center_y, shown.camera_to_world, shown.camera_world, light_sets};
    frontend.backend->present(view, lights, window_width, window_height, overlay);
}

std::optional<Image> renderer_capture() {
    if (!frontend.backend)
        return std::nullopt;
    return frontend.backend->capture();
}

bool renderer_restore(const Image &picture) { return frontend.backend && frontend.backend->restore(picture); }

bool renderer_overlay_start() { return frontend.backend && frontend.backend->start_overlay(); }

void renderer_overlay_stop() {
    if (frontend.backend)
        frontend.backend->stop_overlay();
}

void renderer_release_window() {
    if (frontend.backend)
        frontend.backend->release_window();
}
} // namespace kf
