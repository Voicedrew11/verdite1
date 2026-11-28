#ifndef KF_UI_GAME_WINDOW_ART_H
#define KF_UI_GAME_WINDOW_ART_H
// The gold frame of KF1's own menu windows (the status window's), read from
// the player's disc, for the interface's windows to wear: the tile the game
// mirrors four ways (KF/COM/STAT.DAT's window backdrop, its texels in
// KF/COM/MIX.TIM), cut into a corner and two edge strips that repeat
// seamlessly (each edge's pattern then its mirror), at the game's menu
// brightness. Disc art is never carried in the binary.
//
// The images, by the names the style sheet uses (kf1.rcss, body.game-art):
//   game/frame-corner   the top left corner, frame_border_texels square
//   game/frame-top      the top edge from the corner on, long enough for any window
//   game/frame-left     the left edge from the corner down
// The others are these flipped. Each is drawn frame_upscale times larger,
// pixel for pixel, so it stays sharp when the interface scales it.
// Game thread.
#include <verdite/ui.h>

#include <optional>
#include <string_view>

namespace kf::ext {

inline constexpr int frame_border_texels = 8;
inline constexpr int frame_upscale = 4;

class GameWindowArt final : public verdite::ui::ImageSource {
public:
    // Whether the frame could be read (the disc's files are there); read once.
    [[nodiscard]] bool available() const;
    [[nodiscard]] std::optional<verdite::ui::TextureUpload> load(std::string_view source) const override;
};

// The one the Screen draws from (shell.cpp).
GameWindowArt &game_window_art();

} // namespace kf::ext

#endif // KF_UI_GAME_WINDOW_ART_H
