#include <kf/platform/screen.h>

#include <utility>

namespace kf {
namespace {
GameScreen screen = GameScreen::None;
}

GameScreen host_set_screen(GameScreen next) { return std::exchange(screen, next); }

GameScreen host_screen() { return screen; }
}
