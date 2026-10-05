# AGENTS.md

Guidance for agents working in this repository.

## What this is

A static recompilation of **King's Field** — the first game, released only in
Japan, disc **`SLPS-00017`** — using
[RecompOne](https://github.com/BlackLabelHQ/RecompOne). The series was
renumbered for the West, so the names do not line up:

| Chronological | Japan | North America |
|---|---|---|
| 1st | King's Field (**`SLPS-00017`**) | *not released* |
| 2nd | King's Field II | King's Field (`SLUS-00158`) |
| 3rd | King's Field III | King's Field II (`SLUS-00255`) |

This project targets the **first game**, `SLPS-00017`. The sibling projects
Verdite2 (`SLUS-00158`) and Verdite3 (`SLUS-00255`) target *different games*:
nothing from them — no address, overlay name, finding, patch table or feature
list — applies here, and this disc's addresses do not apply there. Copy
conventions and mechanisms from them, never their facts.

An earlier attempt at this game was made in a clone of Verdite2 (branch
`kf1-port` on this repository's remote, 2026-09-27). Its findings are about this
disc, so they are leads, but it ran on an older runtime and is not this
repository's history: a finding taken from it is re-measured here before it is
written down as one.

There is no decompilation, no ELF and no `.map`. Function boundaries come from a
linear sweep, and PSY-Q library functions are identified with the signature
bank and by hand, so most work here is *reverse engineering*: find an SDK
function's address in the disc image, map it to the runtime's HLE
implementation, re-run the recompiler, run the game, read the logs.

**`NOTES.md` is the index, and `docs/` is the working log.** Read `NOTES.md`
before starting anything, then the one or two documents the task touches:

| file | when |
|---|---|
| `docs/DEVELOPMENT.md` | build, run, diagnose, measure |
| `docs/RECOMPILATION.md` | config, overlays, function maps, SDK addresses |
| `docs/GAME_INTERNALS.md` | the game's own addresses and routines |
| `docs/ENV_VARS.md` | every `KF1_*` switch, in one list |
| `docs/TODO.md` | next steps and open questions |
| `tools/RecompOne/docs/RECOMPONE_PATCHES.md` | every change the fork makes to RecompOne |

Findings go in the right document, not in commit messages and not in this file.

## Build and run

The game does not build without the disc (gitignored, user-supplied, at
`disc/KingsField1.cue`).

```bash
bash scripts/setup_tools.sh        # build the recompiler (tools/RecompOne)

# recompile MIPS -> C# into generated/
dotnet run --project tools/RecompOne/RecompOne.Recompiler -c Release --no-build -- config/kf1.json

dotnet build KingsField1Recomp.csproj -c Release
dotnet bin/Release/net10.0/KingsField1.dll disc/KingsField1.cue
```

The assembly is `KingsField1`, the environment-variable prefix is `KF1_`, and
the command channel's port is 27901 (Verdite2 uses 27900 and Verdite3 27903, so
all three can run). `tools/RecompOne` is a **subtree** of the fork
`Voicedrew11/verdite-recompone`; `tools/verdite-core` is a subtree of the shared,
game-agnostic `Voicedrew11/verdite-core`. `setup_tools.sh` moves them
(`--pull-fork`/`--push-fork`, `--pull-core`/`--push-core`). Their sources are
tracked here, so a fresh clone already has them and nothing needs fetching.

There are no tests. Verification is empirical: run the game with log channels on
and check the trace against what the SDK sequence should look like. The `KF1_*`
switches are listed in `docs/ENV_VARS.md`, and the acceptance test is in
`docs/DEVELOPMENT.md`.

**Anything judged by eye is the user's job, not yours.** Do not capture,
screenshot or otherwise scrape the game window — it burns a lot of context and
produces nothing a person could not say in one sentence. Measure what a counter
can measure, then say plainly what still needs looking at and ask.

## Two traps

These are RecompOne facts, so they hold here regardless of the game:

- **Number bases differ between the config and the CLI.** In `config/kf1.json`,
  `base` is a hex *string* while `size`/`skip`/`offset`/`lba` are decimal numbers;
  on `--generate-function-file`, `-size`/`-skip`/`-offset` are hex.
  `"skip": 2048` in the config is `-skip 800` on the command line.
- **Overlays are read as raw bytes.** `ResolveOverlay` does not parse the PS-X EXE
  header, so every `.EXE` overlay needs `"skip": 2048` to step past the 0x800-byte
  header, and `base` must be the header's real text address. The *boot*
  executable is different: it goes through `Psx/Parser.cs`, which strips the
  header itself.

## The shared subtrees

`tools/RecompOne` and `tools/verdite-core` are subtrees: their sources are
tracked here, so an edit inside one is a change to this repository like any
other.

- **Shared-subtree edits go in their own commits** — a commit that touches a
  shared subtree touches nothing else — and are pushed to that subtree's shared
  repo soon after.
- **This repository's copy must always equal some commit of the shared repo.**
- **A fork or core change must not move Verdite2 or Verdite3.** Both pin the same
  shared commits; a change made for this game is either game-agnostic or gated so
  the others' acceptance tests read the same.
- **Verdite Core's C# (`tools/verdite-core/src/`) compiles into this assembly as
  source**, in namespace `Verdite.Core`, imported by a global using in the csproj.
  `Program.cs` sets the game's tag first (`Game.Configure(tag: "KF1")`), which
  gives core its `[KF1]` log prefix and `KF1_` env prefix. A shared file is
  changed there, in its own commit, not copied back into `patches/`.
- **Nothing goes upstream.** Not a pull request, and not an issue either.
  Upstream rejects AI-authored pull requests. A defect found here is recorded in
  `docs/` and fixed in the vendored tree, which is the point of vendoring it.

## Repository conventions

Never commit disc data or recompiler output — `disc/`, `generated/`, `*.sav` and
`settings.json` are gitignored for copyright and cleanliness reasons.

Commit messages state the *finding*, in the imperative, with the observable
consequence.
