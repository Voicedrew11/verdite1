# Verdite1 (decomp)

King's Field (Japan, 1994, `SLPS-00017`) as a native port, built from
[sushi-shi's decompilation](https://github.com/sushi-shi/kings-field-decomp)
and starting from [acranon's King's Field Enhanced](https://github.com/acranon/kf1-enhanced),
with Verdite1's features being carried over. Nothing from the game is included:
you need your own copy of the disc.

- `decomp/`: the decomp, a submodule (our fork, pinned on `port`). Read-only.
- `patches/`: Verdite's edits to its game code (`tools/patches.py`).
- `port/`: platform layer, renderer, overlay, settings and Lua mods (from kf1-enhanced).
- `third_party/`: Dear ImGui, Lua, cJSON, the GLES headers and loader.

```bash
git submodule update --init decomp
nix develop -c cmake --preset linux
nix develop -c cmake --build --preset linux
mkdir -p data && build/linux/kings-field --disc /path/to/KingsField.cue --extract-to data/en --extract-only
scripts/run-linux.sh
```

Start with [docs/verdite-port.md](docs/verdite-port.md). Modding:
[docs/MODDING.md](docs/MODDING.md). Code from sushi-shi and acranon is CC0.
