# Verdite1

Verdite1 is a PC port of **King's Field** (Japan, 1994 — the first game) built
atop of the [RecompOne](https://github.com/BlackLabelHQ/RecompOne) project. It
is a sibling of [Verdite2](https://github.com/Voicedrew11/verdite2), the port of
King's Field II (released in North America as "King's Field"), and shares its
runtime and most of its enhancements.

## Features

- 60+ fps (any rate up to 240), with the game still running at its own speed:
  the view, creatures and objects are smoothed between the game's 20 fps frames
- Widescreen support (16:9, 16:10, 21:9)
- Perspective-correct textures and corrected vertex wobbling
- Anisotropic filtering and mipmaps
- Ambient occlusion
- Z-buffer
- 24-bit colour
- Enhanced audio quality
- Keyboard and mouse (Escape captures the pointer), and twin-stick gamepad controls
- The game's own memory-card icon on the window

## Status

Early. The game boots, plays its title, starts a New Game and can be walked
around in; most of what Verdite2 adds on top of that is not ported yet (per-pixel
lighting, smooth fog, the map, auto reload, reflections). See `docs/KF1.md`.

## Requirements

A dump of the Japanese PlayStation release (`SLPS-00017`) in `.cue` / `.bin`, or
`.chd` format. The North American "King's Field" (`SLUS-00158`) is the second
game, which Verdite2 ports, and is refused here.

## Credits

Built on [RecompOne](https://github.com/BlackLabelHQ/RecompOne) (MIT). *King's
Field* is the property of FromSoftware; this project ships no game data.
