# TODO

Next steps and open questions. The first task is Phase 2 bring-up: get the game
recompiled and running, following Verdite3's method but applied to this disc.

## Phase 2: bring-up

- ~~Find the executables and their load bases.~~ Done 2026-10-05: no
  `SYSTEM.CNF`, so `PSX.EXE` boots (fork `0090`); see "What is on the disc" in
  `docs/RECOMPILATION.md`.
- ~~Write `config/kf1.json`; sweep the function maps; identify the PSY-Q
  functions~~ (`VSync`, `PutDispEnv` and `DMACallback` by hand).
- ~~Recompile. Boot.~~ 1632 functions, 42 bindings; OPEN.EXE plays the attract.
- ~~Reach an area.~~ Three Starts reach GAME.EXE's first area, which runs at 20
  frames a second once the interrupt poll delivers the vblanks (fork `0091`;
  "The frame gate" in `docs/GAME_INTERNALS.md`). **Not looked at by eye.**
- ~~Walk, and change areas. Save and load through the memory card. Record the
  acceptance test.~~ Done 2026-10-05, by program: see "The acceptance test" in
  `docs/DEVELOPMENT.md`. **By eye, still to do**: the picture, the menus, the
  save and load screens, the area transition.

## Phase 3: the enhancements

- **Frame pacing** (2026-10-05): built and measured, off until judged
  (`KF1_FPS`); menus wait a vblank under it.
- **The camera carried between ticks** (2026-10-05): built and measured, on
  under pacing (`docs/SMOOTHING.md`). So are creatures, objects and their clip
  times, measured on two animated objects; a walking creature is still to
  measure.

Port what Verdite2 and Verdite3 have, mechanism by mechanism, in Verdite3's
order: the agent harness, frame pacing, the camera and models carried between
ticks, the picture (24-bit, perspective, sub-pixel, Z-buffer), widescreen,
keyboard and mouse, the debug mod, and the shipped launcher.
