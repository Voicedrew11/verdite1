#ifndef KF_PLATFORM_SCREEN_H
#define KF_PLATFORM_SCREEN_H
// Which of the game's own screens is up, finer than the input context (which
// is Menu for dialogue and the pause screen too), for Discord. The game code
// sets it around the Cross menu, a shop and a save point's screen.
#include <kf/lib/types.h>

namespace kf {
enum class GameScreen : u8 { None, Menu, Shop, Save };

// Returns the screen it replaces, to be set back when this one closes.
GameScreen host_set_screen(GameScreen screen);
GameScreen host_screen();
}

#endif // KF_PLATFORM_SCREEN_H
