# Environment variables

Every `KF1_*` switch the port reads, in one list, with what each does and its
default, the way Verdite3's `docs/ENV_VARS.md` does. Add an entry here when a
switch is added.

| variable | default | what |
|---|---|---|
| `KF1_LOG` | off | the runtime's log channels: `bios,cd,gpu,dma,sdk,spu,mdec,irq` or `all` |
| `KF1_AGENT` | off | `1`: the state beacon, a JSON line a second (`docs/DEVELOPMENT.md`) |
| `KF1_SHELL` | off | `1` or a port: the command channel on `127.0.0.1:27901` |
| `KF1_AUTOSTART` | off | `new`: Start through the title into a New Game |
| `KF1_AUTOPAD` | off | scripted pad input, `seconds:button:holdMs,…` (`docs/DEVELOPMENT.md`) |
| `KF1_AUTOPAD_FROM` | `game` | the overlay whose load starts `KF1_AUTOPAD`'s clock: `open` or `game` |
| `KF1_FPS` | off | `60`, `144`, … or `off` (uncapped): frame pacing, the picture's rate (`docs/DEVELOPMENT.md`) |
| `KF1_TICKRATE` | `20` | the world's rate under pacing, a comparison only |
| `KF1_FPS_PROBE` | off | `1`: a pacing line a second |
| `KF1_PACING_NOBOUNDARY` | off | `1`: leave `DrawOTag` unhooked, to test the watchdog |
| `KF1_VBLANKPACING` | on | `0`: menus' `VSync` calls on the runtime's clock, a comparison |
| `KF1_VBLANKPACING_PROBE` | off | `1`: a line a second of held `VSync` calls |
| `KF1_RATECENSUS` | off | seconds: the words that change on frames no stage ran |
| `KF1_SMOOTH` | on | `0`: no camera carried between ticks under pacing (`docs/SMOOTHING.md`) |
| `KF1_SMOOTH_PROBE` | off | `1`: a view-smoothing line a second |
| `KF1_SMOOTH_MODELS` | on | `0`: creatures, objects and clips not carried under pacing |
| `KF1_SMOOTH_MODELS_PROBE` | off | `1`: a model-smoothing line a second |
| `KF1_SMOOTH_MODELS_DEBUG` | off | `1`: print each backward pose the probe counts |
