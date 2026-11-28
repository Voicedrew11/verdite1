// The gold frame of KF1's menu windows, from the disc (game_window_art.h)
#include "game_window_art.h"

#include <kf/game/menu.h>
#include <kf/game/resources.h>
#include <kf/platform/language_runtime.h>
#include <kf/renderer/constants.h>
#include <kf/renderer/textures.h>

#include <SDL3/SDL.h>

#include <cstdint>
#include <vector>

namespace kf::ext {
namespace {

constexpr int top_periods = 4;    // the top edge's pattern and its mirror, this many times
constexpr int left_periods = 3;
constexpr int menu_brightness = 96;   // menu_runtime.cpp: MENU_PRIMITIVE_BRIGHTNESS, of 128
constexpr int texel_colour_unity = 128;

// RGBA texels, straight alpha, rows top first.
struct Texels {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;

    [[nodiscard]] const std::uint8_t *at(int x, int y) const {
        return &rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4];
    }
};

struct Art {
    Texels corner, top, left;
};

// The tile the game mirrors into its status window: its top left quarter.
std::optional<Texels> window_tile() {
    const Language language = game_text_language();
    if (!menu_prepare_language(language))
        return std::nullopt;
    const MenuTileSprite &tile = menu_resources().assets.window_backdrop;
    std::vector<u8> tim;
    if (language_file_load(language, "KF/COM/MIX.TIM", tim, disc_import_limit) != FileResult::Ok)
        return std::nullopt;
    TextureStore store;
    Image page;
    if (!texture_store_upload_tim(&store, tim.data(), tim.size()) ||
        !texture_decode(&page, tile.material.source, store.words.data(), store.words.size()))
        return std::nullopt;
    if (tile.width < frame_border_texels * 2 || tile.height < frame_border_texels * 2 ||
        tile.u + tile.width > static_cast<int>(page.width) || tile.v + tile.height > static_cast<int>(page.height))
        return std::nullopt;
    Texels out{tile.width, tile.height, {}};
    out.rgba.reserve(static_cast<std::size_t>(tile.width) * tile.height * 4);
    for (int y = 0; y < tile.height; ++y) {
        const std::uint8_t *row = page.rgba.data() + ((static_cast<std::size_t>(tile.v + y) * page.width) + tile.u) * 4;
        out.rgba.insert(out.rgba.end(), row, row + static_cast<std::size_t>(tile.width) * 4);
    }
    return out;
}

// A rectangle of the tile.
Texels cut(const Texels &from, int x, int y, int width, int height) {
    Texels out{width, height, {}};
    for (int row = 0; row < height; ++row)
        for (int column = 0; column < width; ++column) {
            const std::uint8_t *texel = from.at(x + column, y + row);
            out.rgba.insert(out.rgba.end(), texel, texel + 4);
        }
    return out;
}

enum class Along : unsigned char { Across, Down };

// The pattern then its mirror, periods times: seamless where copies meet.
Texels mirrored_strip(const Texels &pattern, int periods, Along along_axis) {
    const bool across = along_axis == Along::Across;
    const int period = (across ? pattern.width : pattern.height) * 2;
    Texels out{across ? period * periods : pattern.width, across ? pattern.height : period * periods, {}};
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x) {
            const int along = (across ? x : y) % period;
            const int length = across ? pattern.width : pattern.height;
            const int from = along < length ? along : period - 1 - along;
            const std::uint8_t *texel = across ? pattern.at(from, y) : pattern.at(x, from);
            out.rgba.insert(out.rgba.end(), texel, texel + 4);
        }
    return out;
}

std::optional<Art> read_art() {
    const std::optional<Texels> tile = window_tile();
    if (!tile) {
        return std::nullopt;
    }
    const int b = frame_border_texels;
    return Art{cut(*tile, 0, 0, b, b),
               mirrored_strip(cut(*tile, b, 0, tile->width - b, b), top_periods, Along::Across),
               mirrored_strip(cut(*tile, 0, b, b, tile->height - b), left_periods, Along::Down)};
}

// Read once it can be: until the disc's files are there (a first start), tried
// again every retry_ms.
const std::optional<Art> &art() {
    constexpr std::uint64_t retry_ms = 2000;
    static std::optional<Art> read;
    static std::optional<std::uint64_t> tried_ms;
    const std::uint64_t now_ms = SDL_GetTicks();
    if (!read && (!tried_ms || now_ms - *tried_ms >= retry_ms)) {
        tried_ms = now_ms;
        read = read_art();
    }
    return read;
}

// Upscaled pixel for pixel, at the menu's brightness, premultiplied. A
// semi-transparent texel is the game's half blend.
verdite::ui::TextureUpload upload(const Texels &texels) {
    verdite::ui::TextureUpload out;
    out.width_px = texels.width * frame_upscale;
    out.height_px = texels.height * frame_upscale;
    out.rgba.resize(static_cast<std::size_t>(out.width_px) * static_cast<std::size_t>(out.height_px) * 4);
    for (int y = 0; y < out.height_px; ++y)
        for (int x = 0; x < out.width_px; ++x) {
            const std::uint8_t *texel = texels.at(x / frame_upscale, y / frame_upscale);
            const int alpha = texel[3] == 0 ? 0 : texel[3] == texture_alpha_semitransparent_marker ? 128 : 255;
            std::uint8_t *pixel = &out.rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(out.width_px) +
                                             static_cast<std::size_t>(x)) * 4];
            for (int c = 0; c < 3; ++c)
                pixel[c] = static_cast<std::uint8_t>(texel[c] * menu_brightness / texel_colour_unity * alpha / 255);
            pixel[3] = static_cast<std::uint8_t>(alpha);
        }
    return out;
}

} // namespace

bool GameWindowArt::available() const {
    return art().has_value();
}

std::optional<verdite::ui::TextureUpload> GameWindowArt::load(std::string_view source) const {
    const Texels *texels = nullptr;
    if (source == "game/frame-corner")
        texels = art() ? &art()->corner : nullptr;
    else if (source == "game/frame-top")
        texels = art() ? &art()->top : nullptr;
    else if (source == "game/frame-left")
        texels = art() ? &art()->left : nullptr;
    if (!texels)
        return std::nullopt;
    return upload(*texels);
}

GameWindowArt &game_window_art() {
    static GameWindowArt instance;
    return instance;
}

} // namespace kf::ext
