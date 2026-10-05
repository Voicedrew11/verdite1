# TODO

Next steps and open questions. The first task is Phase 2 bring-up: get the game
recompiled and running, following Verdite3's method but applied to this disc.

## Phase 2: bring-up

- Use `tools/verdite-core/scripts/inspect_disc.py` and `extract_file.py` to find
  the executables and their load bases. The earlier attempt (`kf1-port`) read no
  `SYSTEM.CNF` on this disc, so the BIOS default `PSX.EXE` boots: check it, and
  what the recompiler does without one.
- Write `config/kf1.json`, declaring the overlays and their addresses.
- Sweep a function map per executable into `config/funcmaps/`.
- Identify the PSY-Q functions with the signature bank, and by hand where this
  1994 library is older than the bank's.
- Recompile.
- Boot.
- Reach gameplay.
- Save and load through the memory card.
- Record the acceptance test.

## Phase 3: the enhancements

Port what Verdite2 and Verdite3 have, mechanism by mechanism, in Verdite3's
order: the agent harness, frame pacing, the camera and models carried between
ticks, the picture (24-bit, perspective, sub-pixel, Z-buffer), widescreen,
keyboard and mouse, the debug mod, and the shipped launcher.
