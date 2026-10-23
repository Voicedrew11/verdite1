# AGENTS.md

Branch `decomp` of Verdite1: King's Field (JP, `SLPS-00017`) as native C/C++
from sushi-shi's decompilation, with Verdite1's features being carried over.
It shares nothing with `main` (the RecompOne recompilation); `main`'s
AGENTS.md, NOTES.md and `KF1_*` switches do not apply here. The layout is
holyland's.

Read `docs/verdite-port.md` first: the direction (every Verdite port becomes a
decomp), the layout, build, the feature inventory and the order. Then, as the
task needs: `docs/RENDERER_PLAN.md` (platforms, Vulkan, tiers, carrying the
picture over), `docs/RUNTIME.md` (what is emulated, the 3DS budget, the
frame-rate gap), `decomp/docs/port-notes.md` (the decomp's own notes).

## The decomp is read-only

- Never edit `decomp/` (the submodule: our fork of sushi-shi's decomp, pinned
  on `port`) or anything under `build/<preset>/decomp_src/` (the patched copy
  that is compiled; compiler errors point into it). Configure fails if
  `decomp/` has local changes, and `decomp_src/` is read-only.
- Change game code only in `work/`: `python3 tools/patches.py apply`, edit
  `work/src/...` or `work/include/...`, then `python3 tools/patches.py save`
  and commit `patches/`. The same path below `work/` as below `decomp_src/` is
  the file to edit.
- Never hand-edit the files in `patches/`; `patches.py save` writes them.
- Move the pin with `python3 tools/patches.py upgrade <commit>`, then update
  the `rev` and `hash` in `flake.nix`. The decomp has no `symbol_addrs.txt`,
  so renames come up as conflicts or compile errors.
- `port/` (platform, renderer, overlay, settings, mods) and `third_party/` are
  ordinary source, edited in place.
- Nothing goes upstream to sushi-shi or acranon: no pull requests, no issues.
  A fix to the reconstruction is a patch here.

## Build and run

- `git submodule update --init decomp` after cloning.
- Build: `nix develop -c cmake --preset linux && nix develop -c cmake --build --preset linux`.
- Run: `scripts/run-linux.sh [--skip-intro]` (data in the gitignored `data/en`).
- Never commit disc data (`data/`, images, `.ppf`, the English delta).
- Anything judged by eye is the user's job: do not screenshot the window.
- Commit messages state the finding, in the imperative, with the consequence.
