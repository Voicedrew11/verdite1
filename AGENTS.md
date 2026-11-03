# AGENTS.md

**Writing C++?** Follow `docs/cpp-reference.md` (the good/bad rules, and its "Before finishing"
list before calling it done) and `docs/code-style.md` (how this repo's modules look). Both are for
our code (`libverdite/`, `port/`); patches to the decomp's game code follow the decomp's style.

Branch `decomp` of Verdite1: King's Field (JP, `SLPS-00017`) as native C/C++
from sushi-shi's decompilation, with Verdite1's features being carried over.
It shares nothing with `main` (the RecompOne recompilation); `main`'s
AGENTS.md, NOTES.md and `KF1_*` switches do not apply here. The layout is
holyland's.

Read `docs/verdite-port.md` first: the direction (every Verdite port becomes a
decomp), the layout, build, the feature inventory and the order. Then, as the
task needs: `docs/RENDERER_PLAN.md` (platforms, Vulkan, tiers, carrying the
picture over), `docs/RUNTIME.md` (what is emulated, the 3DS budget, the
frame-rate gap), `docs/ANDROID_PLAN.md` (the Android port; a draft), `docs/NETWORKING_PLAN.md` (online play; a draft), `docs/COOP_PLAN.md` (coop's model; a draft), `docs/PLAYER_MODEL.md` (the other players' knight model and its tools), `docs/RAW_LAYER_PLAN.md` (the modding API's raw layer: a handoff), `docs/CALL_LAYER_PLAN.md` (the modding API's calls into the game: a plan), `docs/HOOK_LAYER_PLAN.md` (the modding API's hooks on the game's functions), `docs/CONTENT_LAYER_PLAN.md` (the modding API's new items, monster kinds and floors), `docs/FUZZING_PLAN.md` (the fuzzer: what is
done and what comes next; running it is "Fuzzing" in `verdite-port.md`),
`docs/PACKAGING.md` (the packages, releases, previews and the update notice),
`decomp/docs/port-notes.md` (the decomp's own notes).

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
- The game is compiled from `build/<preset>/hook_src/`, a copy of `decomp_src/` with hookable functions renamed `kf_game_*` (`docs/HOOK_LAYER_PLAN.md`). It is generated, and `#line` keeps messages and debuggers on `decomp_src/`.
- Move the pin with `python3 tools/patches.py upgrade <commit>`, then update
  the `rev` and `hash` in `flake.nix`. The decomp has no `symbol_addrs.txt`,
  so renames come up as conflicts or compile errors.
- `libverdite/` is the game-agnostic C++ library the Verdite ports will share
  (the successor to the C# Verdite Core), kept in this repo for now: no `kf/` or decomp include in it, and its changes in their own commits.
- `port/` (platform, renderer, overlay, settings, mods) and `third_party/` are
  ordinary source, edited in place.
- Nothing goes upstream to sushi-shi or acranon: no pull requests, no issues.
  A fix to the reconstruction is a patch here.

## Code quality

- Write new C++ by `docs/code-style.md`; patches to game code follow the decomp's style.
- **Checks run ONLY before a push to the remote. Never before a commit, a merge, or to call work done, and never in a plan
  for work that is not being pushed.** That covers the fuzzer, the sanitizer builds, the lint and the smoke runs, not just
  `check.sh`. It overrides any general rule to run checks before finishing, global instructions included. Until a push, build
  and run what the task itself needs, and say plainly in the report that the checks did not run.
- Before a push, run `scripts/check.sh --base @{upstream}`, so the lint covers every commit being pushed (`--quick`
  skips the sanitizer build). It builds, runs
  libverdite's tests, lints our code (`scripts/lint.py`: all of `libverdite/`, the changed lines
  of `port/`, the lines the patches add) and boots the game headless, plain and under ASan/UBSan.
- A finding is fixed, not suppressed; a check turned off goes in the table in `code-style.md`.

## Build and run

- `git submodule update --init decomp` after cloning.
- Build: `nix develop -c cmake --preset linux && nix develop -c cmake --build --preset linux`.
- Run: `scripts/run-linux.sh [--skip-intro]` (data in the gitignored `data/en`).
- Never commit disc data (`data/`, images, `.ppf`, the English delta).
- How the game looks is the user's call, above all in motion, which stills and numbers cannot show: do not screenshot the window or say a visual change looks right. Check what can be measured and name what to look at. For model and art work, render stills offline and fix what they show before handing over.
- Commit messages: an imperative subject under 72 characters, a blank line, then a short body with the finding and the consequence.
