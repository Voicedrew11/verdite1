#ifndef KF_RENDERER_CONSTANTS_H
#define KF_RENDERER_CONSTANTS_H

namespace kf {
inline constexpr int render_width = 320;
inline constexpr int render_height = 240;
inline constexpr int texture_store_width = 1024;
inline constexpr int texture_store_height = 512;
inline constexpr int texture_page_extent = 256;
inline constexpr float texture_uv_scale = 256.0f;
inline constexpr float texture_color_unity = 128.0f;
inline constexpr float color8_scale = 255.0f;
inline constexpr int color8_max = 255;
inline constexpr int color5_bits = 5;
inline constexpr int color5_max = 31;
inline constexpr int color5_color8_shift = 3;
inline constexpr int color5_replication_shift = 2;
inline constexpr int texture_semitransparent_bit = 0x8000;
inline constexpr int texture_alpha_semitransparent_marker = 128;
inline constexpr int texture_alpha_opaque_marker = 255;

// Packed texture-page and palette selectors in the original resource formats.
inline constexpr int texture_page_x_mask = 0x0f;
inline constexpr int texture_page_x_stride = 64;
inline constexpr int texture_page_y_mask = 0x10;
inline constexpr int texture_page_y_scale = 16;
inline constexpr int texture_blend_shift = 5;
inline constexpr int texture_blend_mask = 3;
inline constexpr int texture_format_shift = 7;
inline constexpr int texture_format_mask = 3;
inline constexpr int palette_x_mask = 0x3f;
inline constexpr int palette_x_stride = 16;
inline constexpr int palette_y_shift = 6;
inline constexpr int packed_uv_component_bits = 8;
inline constexpr int packed_uv_component_mask = 0xff;
}

#endif // KF_RENDERER_CONSTANTS_H
