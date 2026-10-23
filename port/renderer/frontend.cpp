#include <kf/renderer/backend.h>
#include <kf/renderer/frontend.h>

#include "frame_batch.h"
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
    PictureOptions options;            // as the settings asked; within_tier() is what draws
    TextureStore textures;
    TextureLayers layers;
    std::uint64_t textures_seen = 0;   // TextureStore::generation the layers were decoded from
    ViewSmoothing smoothing;
    std::vector<GameLight> lights;
    int logical_width = render_width;
    int scale = 1;
};
Frontend frontend;

// Options above the backend's tier are off there.
PictureOptions within_tier(PictureOptions options, Tier tier) {
    const PictureOptions off{};
    if (tier < Tier::Enhanced) {
        options.dynamic_lights = off.dynamic_lights;
        options.light_occlusion = off.light_occlusion;
        options.shadows = off.shadows;
        options.ambient_occlusion = off.ambient_occlusion;
        options.glow = off.glow;
        options.fxaa = off.fxaa;
    }
    if (tier < Tier::Clean) {
        options.true_color = off.true_color;
        options.depth_buffer = off.depth_buffer;
        options.texture_filter = off.texture_filter;
        options.mipmaps = off.mipmaps;
        options.smoothing = off.smoothing;
        options.supersample = off.supersample;
    }
    return options;
}

PictureOptions drawn_options() { return within_tier(frontend.options, frontend.backend->tier()); }

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
    frontend.layers.begin_update();
    const FrameBatch batch = batch_faces(frame.faces, rules, frontend.layers);
    upload_new_layers();
    frontend.backend->upload_frame(batch.vertices, batch.runs, frame.style);
    frontend.smoothing.take_update(frame.camera, frame.look_applied, frame.entities, batch.entity_tags, at_ns);
    frontend.lights.assign(frame.lights.begin(), frame.lights.end());
}

void renderer_present(std::uint64_t now_ns, const SVECTOR &pending_look, int window_width, int window_height,
                      ImDrawData *overlay) {
    if (!frontend.backend)
        return;
    const PictureOptions options = drawn_options();
    const ShownView shown = options.smoothing ? frontend.smoothing.view_at(now_ns, pending_look)
                                              : frontend.smoothing.view_as_drawn();
    std::vector<RenderLight> lights;
    if (options.dynamic_lights)
        lights = shown_lights(frontend.lights, options.light_strength, shown.motions.front(), now_ns);
    const FrameView view{shown.motions,     shown.reproject, !render_subpixel, shown.projection_distance,
                         shown.center_x,    shown.center_y,  shown.camera_to_world, shown.camera_world};
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
} // namespace kf
