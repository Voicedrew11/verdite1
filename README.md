<img width="700" alt="verdite1 logo" src="https://github.com/user-attachments/assets/578f8f59-3e5e-4563-86eb-ffa44809b3ee" />

***

[![Discord](https://img.shields.io/discord/1553561882843947149?label=Discord&logo=discord&logoColor=white&color=5865F2)](https://discord.gg/R4EKCs8RWH)
[![Release](https://img.shields.io/github/v/release/Voicedrew11/verdite1)](https://github.com/Voicedrew11/verdite1/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/Voicedrew11/verdite1/total)](https://github.com/Voicedrew11/verdite1/releases)

Verdite1 is a port of King's Field (JP) to Windows, Linux and Android, built as native C/C++ from [sushi-shi's decompilation](https://github.com/sushi-shi/kings-field-decomp), starting from [acranon's King's Field Enhanced](https://github.com/acranon/kf1-enhanced). This is not to be confused with King's Field (US), or King's Field II (JP), which is a different game covered by [verdite2](https://github.com/Voicedrew11/verdite2)

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

## Install

Get the package for your system from the [latest release](https://github.com/Voicedrew11/verdite1/releases/latest). The game asks for your disc image on first launch, and tells you when a new version is out.

- **Windows:** run `Verdite1-*-setup.exe` (no administrator needed), or unpack the `.zip` anywhere.
- **Linux:** make the `.AppImage` executable and run it. It adds itself to your desktop's menu.
- **Android:** install the `.apk` (arm64, Android 10 or later), then choose your disc image in the game.

## Credits

Built on [sushi-shi's decompilation](https://github.com/sushi-shi/kings-field-decomp) and [King's Field Enhanced](https://github.com/acranon/kf1-enhanced) (both CC0). *King's
Field* is the property of FromSoftware; this project ships no game data.
