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
