# AGENTS.md

Branch `decomp` of Verdite1: King's Field (JP, `SLPS-00017`) as native C/C++
from sushi-shi's decompilation, by way of acranon's kf1-enhanced, with
Verdite1's features being carried over. It shares no history with `main`
(the RecompOne recompilation); `main`'s AGENTS.md, NOTES.md and `KF1_*`
switches do not apply here.

Read `docs/verdite-port.md` first: the direction (every Verdite port becomes a
decomp), remotes, build, the feature inventory and the order. Then, as the task
needs: `docs/RENDERER_PLAN.md` (platforms, Vulkan, tiers, carrying the
picture over), `docs/RUNTIME.md` (what is emulated, the 3DS budget, the
frame-rate gap), `docs/port-notes.md` (the decomp's own technical notes).

- Build: `nix develop -c cmake --preset linux && nix develop -c cmake --build --preset linux`.
- Run: `scripts/run-linux.sh [--skip-intro]` (data in the gitignored `data/en`).
- Nothing goes upstream to sushi-shi or acranon: no pull requests, no issues.
- Never commit disc data (`data/`, images, `.ppf`).
- Anything judged by eye is the user's job: do not screenshot the window.
- Commit messages state the finding, in the imperative, with the consequence.
