<img width="700" alt="verdite1 logo" src="https://github.com/user-attachments/assets/578f8f59-3e5e-4563-86eb-ffa44809b3ee" />

***

[![Discord](https://img.shields.io/discord/1553561882843947149?label=Discord&logo=discord&logoColor=white&color=5865F2)](https://discord.gg/R4EKCs8RWH)

Verdite1 is a PC port of King's Field (JP). This branch, `decomp`, builds it as native C/C++ from [sushi-shi's decompilation](https://github.com/sushi-shi/kings-field-decomp), starting from [acranon's King's Field Enhanced](https://github.com/acranon/kf1-enhanced), with Verdite1's features being carried over. The released builds come from `main`, on [RecompOne](https://github.com/BlackLabelHQ/RecompOne). This is not to be confused with King's Field (US), or King's Field II (JP), which is a different game covered by [verdite2](https://github.com/Voicedrew11/verdite2)

## Features

- Built in support for English translation
- Widescreen support (16:9, 16:10, 21:9)
- Perspective-correct textures and corrected vertex wobbling
- Anisotropic filtering
- Ambient occlusion
- Z-buffer
- 24-bit colour
- 60+ fps
- Per-pixel lighting
- Smooth fog
- Keyboard and mouse support
- Mod support
- Discord Rich Presence

## Status

In development. Features from `main` are still being carried over; the list is in [docs/verdite-port.md](docs/verdite-port.md), which also covers building.

## Requirements

A dump of the Japanese PlayStation release (`SLPS-00017`) in `.cue` / `.bin`, `.iso` or `.chd` format.

## Credits

Built on [sushi-shi's decompilation](https://github.com/sushi-shi/kings-field-decomp) and [King's Field Enhanced](https://github.com/acranon/kf1-enhanced) (both CC0). *King's
Field* is the property of FromSoftware; this project ships no game data.
