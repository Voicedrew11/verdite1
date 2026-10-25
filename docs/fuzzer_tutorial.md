What to run

┌──────────────────┬────────────────┬──────────────────────────────────────────────────────────────────────────────────────────┐
│       flag       │    default     │                                       what it does                                       │
├──────────────────┼────────────────┼──────────────────────────────────────────────────────────────────────────────────────────┤
│ --runs N         │ 12             │ how many seeds                                                                           │
├──────────────────┼────────────────┼──────────────────────────────────────────────────────────────────────────────────────────┤
│ --seed-base S    │ 100            │ the first seed; runs use S, S+1, …                                                       │
├──────────────────┼────────────────┼──────────────────────────────────────────────────────────────────────────────────────────┤
│ --budget SECONDS │ —              │ keep launching seeds for this long, instead of --runs                                    │
├──────────────────┼────────────────┼──────────────────────────────────────────────────────────────────────────────────────────┤
│ --jobs J         │ half your CPUs │ runs at once                                                                             │
├──────────────────┼────────────────┼──────────────────────────────────────────────────────────────────────────────────────────┤
│ --ticks T        │ 72000          │ game-clock ticks per run, 60 a second (72000 is 20 game minutes)                         │
├──────────────────┼────────────────┼──────────────────────────────────────────────────────────────────────────────────────────┤
│ --timeout S      │ 900            │ wall-clock seconds before a run is killed                                                │
├──────────────────┼────────────────┼──────────────────────────────────────────────────────────────────────────────────────────┤
│ --preset P       │ linux          │ which build: linux, sanitize or fuzz (fuzz is ASan, UBSan and bounds-checked containers) │
├──────────────────┼────────────────┼──────────────────────────────────────────────────────────────────────────────────────────┤
│ --binary PATH    │ —              │ a binary other than build/<preset>/verdite1                                              │
├──────────────────┼────────────────┼──────────────────────────────────────────────────────────────────────────────────────────┤
│ --data DIR       │ data/en        │ the extracted game files                                                                 │
├──────────────────┼────────────────┼──────────────────────────────────────────────────────────────────────────────────────────┤
│ --out DIR        │ fuzz/          │ where runs and the report go                                                             │
└──────────────────┴────────────────┴──────────────────────────────────────────────────────────────────────────────────────────┘

How a run plays

┌─────────────────────────────┬────────────────────────────────────────────────────────────────────────────────────────────────────┐
│            flag             │                                            what it does                                            │
├─────────────────────────────┼────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ --mix                       │ each seed picks its own start: floor, kit, strength, macro rate, godmode                           │
├─────────────────────────────┼────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ --vary                      │ each seed picks gameplay and display settings: Classic mode, tick rate, enemy turn, aspect, …      │
├─────────────────────────────┼────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ --area 1-5|random           │ start on that floor, in an open cell                                                               │
├─────────────────────────────┼────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ --kit                       │ start with one of every item and spells 0–8                                                        │
├─────────────────────────────┼────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ --strong                    │ start with max HP anP bugs still show)                                │
├─────────────────────────────┼───────────────────────────────────────────────────────────────────────┤
│ --godmode                   │ HP topped up every uead of dying                                      │
├─────────────────────────────┼───────────────────────────────────────────────────────────────────────┤
│ --load CARD                 │ start by quick loadi0017KF.quick)                                     │
├─────────────────────────────┼────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ --no-macros                 │ no warps, jumps, sav                                                  │
├─────────────────────────────┼────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ --macro-rate PCT            │ scales every macro'sormal)                                            │
├─────────────────────────────┼────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ --weights name=w,...        │ reweights presses or macros by name (lists below)                                                  │
├─────────────────────────────┼───────────────────────────────────────────────────────────────────────┤
│ --skip a,b                  │ turns invariants off cell, in-rock, items, equipment, actors, effects │
├─────────────────────────────┼───────────────────────────────────────────────────────────────────────┤
│ --stop                      │ ends a run at its fi                                                  │
├─────────────────────────────┼────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ --game-seed N               │ the game's own randoed)                                               │
├─────────────────────────────┼────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ --setting section.key=value │ a settings.json value for every run, e.g. gameplay.classic_mode=true (repeatable)                  │
├─────────────────────────────┼───────────────────────────────────────────────────────────────────────┤
│ --settings FILE             │ a whole settings.jso                                                  │
├─────────────────────────────┼───────────────────────────────────────────────────────────────────────┤
│ --env KEY=VALUE             │ extra environment va                                                  │
├─────────────────────────────┼───────────────────────────────────────────────────────────────────────┤
│ --trace                     │ writes each run's state trace (trace.txt)                                                          │
├─────────────────────────────┼───────────────────────────────────────────────────────────────────────┤
│ --status TICKS              │ ticks between status                                                  │
└─────────────────────────────┴───────────────────────────────────────────────────────────────────────┘

Names for --weights:
- Play presses: walk, walk-left, walk-right, back-up, turn-left, turn-right, strafe-left, strafe-right, look-up, look-down, attack,
  magic, examine, menu, start, pause, idle.
- Menu presses: menu-down, menu-up, menu-right, menu-left, menu-confirm, menu-back.
- Macros (the weight is the chance per 10,000 update

Modes

┌─────────────────────────────────┬──────────────────────────────────────────────────────────────────┐
│              flag               │                 it does                                          │
├─────────────────────────────────┼──────────────────────────────────────────────────────────────────┤
│ --replay RUN_DIR                │ plays a run's recording back exactly, with its switches and settings                          │
├─────────────────────────────────┼──────────────────────────────────────────────────────────────────┤
│ --windowed                      │ with --replay: in a window at normal speed, so you can watch it                               │
├─────────────────────────────────┼───────────────────────────────────────────────────────────────────────────────────────────────┤
│ --minimise RUN_DIR              │ cuts a run's rechat still give its finding (--max-tries N, 200)  │
├─────────────────────────────────┼──────────────────────────────────────────────────────────────────┤
│ --check-determinism             │ runs each seed tatch                                             │
├─────────────────────────────────┼──────────────────────────────────────────────────────────────────┤
│ --compare-setting               │ runs each seed with and without that setting; game state must match. Alone, it tries all 14   │
│ [section.key=value]             │ visual settings                                                  │
├─────────────────────────────────┼───────────────────────────────────────────────────────────────────────────────────────────────┤
│ --coverage-only DIR             │ prints a fuzz folder's merged coverage, runs nothing                                          │
├─────────────────────────────────┼───────────────────────────────────────────────────────────────────────────────────────────────┤
│ --targets [NAME...]             │ the libFuzzer tamory_card, save_card, disc_image, codecs (none   │
│                                 │ named means all)                                                 │
├─────────────────────────────────┼───────────────────────────────────────────────────────────────────────────────────────────────┤
│ --seconds S                     │ with --targets:                                                  │
├─────────────────────────────────┼──────────────────────────────────────────────────────────────────┤
│ --max-len N                     │ with --targets: the longest input                                                             │
├─────────────────────────────────┼──────────────────────────────────────────────────────────────────┤
│ --corpus-from PATH              │ with --targets: more seed files or globs (repeatable)                                         │
└─────────────────────────────────┴───────────────────────────────────────────────────────────────────────────────────────────────┘

Output: --keep-all keeps every file of clean runs (srops the line printed per run.

Starter recipes

1. A first look (about a minute)
python3 scripts/fuzz.py --runs 6 --ticks 20000

2. Broad coverage (about 5 minutes): every seed starhis is the run that reached all five floors.
python3 scripts/fuzz.py --runs 48 --mix --vary

3. Realistic play: the pad only, no teleporting. Sloer it finds a player could hit.
python3 scripts/fuzz.py --runs 24 --no-macros

4. Deep floors and fights: start anywhere, equipped, attacking more.
python3 scripts/fuzz.py --runs 24 --area random --ki00,magic=150

5. Classic mode: checks the faithful mode on its own there, so those macros don't fire.
python3 scripts/fuzz.py --runs 24 --setting gameplay

6. Under the sanitizers: catches memory errors, undefined behaviour and out-of-range indexes. Build it once with nix develop -c cmake
--build --preset fuzz. It's several times slower, so timeout.
python3 scripts/fuzz.py --preset fuzz --runs 24 --mix --vary --ticks 36000 --timeout 3600

7. Do visual settings change gameplay? 14 settings ×
python3 scripts/fuzz.py --compare-setting --runs 2 -

8. Files from outside the game: the libFuzzer target
python3 scripts/fuzz.py --targets --seconds 600 --max-len 131072

9. Overnight: 8 hours of seeds under the sanitizers.
python3 scripts/fuzz.py --preset fuzz --budget 28800-timeout 3600

When a run finds something, fuzz/report.md gives the exact command for each finding:
python3 scripts/fuzz.py --replay fuzz/runs/000117   n, exactly
python3 scripts/fuzz.py --replay fuzz/runs/000117 --
python3 scripts/fuzz.py --minimise fuzz/runs/000117 at still breaks
