#!/usr/bin/env python3
"""Play the game many times at once, headless and seeded, and report what broke.

    python3 scripts/fuzz.py --runs 36                      # six at a time, 72000 ticks each
    python3 scripts/fuzz.py --budget 3600 --mix --vary     # an hour of seeds, each set up differently
    python3 scripts/fuzz.py --runs 1 --seed-base 117       # one seed again
    python3 scripts/fuzz.py --replay fuzz/runs/000117      # its recording, pressed back exactly
    python3 scripts/fuzz.py --replay fuzz/runs/000117 --windowed   # ... in a window, at normal speed
    python3 scripts/fuzz.py --check-determinism --runs 6   # each seed twice: the traces must agree
    python3 scripts/fuzz.py --compare-setting display.aspect=16:9 --runs 6   # a setting that must not change play
    python3 scripts/fuzz.py --minimise fuzz/runs/000117    # the shortest recording that still breaks
    python3 scripts/fuzz.py --preset sanitize --runs 12    # under ASan and UBSan
    python3 scripts/fuzz.py --coverage-only fuzz           # merge the runs' coverage, run nothing
    python3 scripts/fuzz.py --targets --seconds 600        # the libFuzzer targets, not the game (fuzz preset)
    python3 scripts/fuzz.py --targets save_card --corpus-from ~/saves/BISLPS-00017KF

Each run is the built game (build/<preset>/verdite1; build it first) with --headless --turbo
--skip-intro and KF_FUZZ=<seed>, in a directory of its own under fuzz/runs/ with its own saves
folder and its own user folder (XDG_DATA_HOME), so settings, crash reports and mods are the
run's, not the player's. port/platform/fuzz.cpp plays (the policy is libverdite's
verdite/fuzz.h) and prints what it finds as [KF1-FUZZ] lines; this script launches, watches,
classifies and groups:

    crash      a fatal signal, an assertion, or a sanitizer report
    fail       the game stopped itself (host_fail: exit 1 with a message)
    violation  an invariant broke (fuzz.cpp: vitals, in-rock, equipment, actors, stall, ...)
    timeout    the wall-clock limit; the process is killed
    ok         ran its ticks out and exited 0

Findings group by signature (a crash's top stack frames, a sanitizer's first line and frame, a
failure's message, a violation's kind) into fuzz/report.md and report.json, each with its seeds
and the command that reproduces it. Under turbo the game is deterministic, so a recording
(fuzz-input.log) replays a run exactly and a seed plays the same run again. See
docs/FUZZING_PLAN.md and "Fuzzing" in docs/verdite-port.md.
"""

from __future__ import annotations

import argparse
import json
import os
import random
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DATA = REPO / "data/en"
USER_FOLDER = Path("home/Verdite/Verdite1")   # inside a run directory, under XDG_DATA_HOME
QUICK_CARD = "BISLPS-00017KF.quick"
ENV_FILE = "fuzz-env.json"
KEPT_ON_CLEAN = ("log.txt", "fuzz-input.log", "fuzz-coverage.txt", "trace.txt", ENV_FILE)
TICKS_PER_SECOND = 60

# --vary: settings that change what the game does, each with the values tried; None leaves
# the default. Written to the run's settings.json; replayed by --replay.
VARY = {
    "gameplay.classic_mode": [None, True],
    "gameplay.tick_rate": [None, 15, 20],
    "gameplay.enemy_turn": [None, 1.0, 2.0, 4.0],
    "gameplay.enemy_track": [None, False],
    "gameplay.fix_ceiling_fall": [None, False],
    "gameplay.fix_spatial_panning": [None, False],
    "gameplay.auto_use_keys": [None, True],
    "display.aspect": [None, "4:3", "21:9"],
    "display.hud_at_edges": [None, False],
    "map.minimap": [None, False],
}

# --compare-setting with no value: the settings that claim to change only the picture. Each must
# leave the game's state exactly as it was, update by update.
VISUAL_SETTINGS = (
    "display.aspect=4:3", "display.aspect=21:9", "display.hud_at_edges=false", "display.frame_rate=20",
    "display.resolution=1", "display.true_color=false", "display.subpixel=false", "display.perspective=false",
    "display.depth_buffer=false", "display.fog=0", "display.detailed_map=false", "display.dynamic_lights=false",
    "map.minimap=false", "overlay.item_descriptions=false",
)

FUZZ_RE = re.compile(r"^\[KF1-FUZZ\] (\{.*\})$")
CRASH_FRAME_RE = re.compile(r"^#\d+ (.+)$")
SANITIZER_RE = re.compile(r"(ERROR: \w+Sanitizer: [\w-]+|runtime error: .*)")
SANITIZER_FRAME_RE = re.compile(r"#\d+ 0x[0-9a-f]+ in (\S+)")
SANITIZER_SITE_RE = re.compile(r"(\S+\.(?:cpp|h|c|hpp)):(\d+)(?::\d+)?: runtime error")
ASSERT_RE = re.compile(r"(Assertion '.*' failed\.?)")
# Frames that say where a crash was handled, not where it happened.
PLUMBING = ("verdite::", "__", "abort", "raise", "std::__", "_start", "main", "cpptrace", "__sanitizer")


@dataclass
class Run:
    seed: int
    dir: Path
    proc: subprocess.Popen | None = None
    started: float = 0.0
    killed: str | None = None
    exit: int | None = None
    outcome: str = "ok"
    findings: list[dict] = field(default_factory=list)
    end: dict = field(default_factory=dict)
    seconds: float = 0.0
    extra_env: dict = field(default_factory=dict)
    settings: dict = field(default_factory=dict)
    label: str = ""


def rel(p: Path) -> str:
    p = Path(p).resolve()
    return str(p.relative_to(REPO)) if p.is_relative_to(REPO) else str(p)


def binary(args) -> Path:
    return Path(args.binary) if args.binary else REPO / "build" / args.preset / "verdite1"


def stale_build(args) -> str | None:
    exe = binary(args)
    if not exe.exists():
        return f"{rel(exe)} is missing; nix develop -c cmake --build --preset {args.preset}"
    built = exe.stat().st_mtime
    sources = [*REPO.glob("port/**/*.cpp"), *REPO.glob("port/**/*.h"), *REPO.glob("libverdite/**/*.cpp"),
               *REPO.glob("libverdite/include/**/*.h"), *REPO.glob("patches/**/*.patch")]
    newer = [p for p in sources if p.stat().st_mtime > built + 1]
    if newer:
        return f"{len(newer)} source file(s) newer than {rel(exe)} (e.g. {rel(newer[0])})"
    return None


# --- settings.json: "section.key=value" written into the run's user folder

def parse_value(text: str):
    lowered = text.lower()
    if lowered in ("true", "false"):
        return lowered == "true"
    for kind in (int, float):
        try:
            return kind(text)
        except ValueError:
            pass
    return text


def parse_setting(item: str) -> tuple[str, object]:
    key, eq, value = item.partition("=")
    if not eq or "." not in key:
        raise SystemExit(f"--setting {item!r}: expected section.key=value")
    return key, parse_value(value)


def write_settings(run_dir: Path, base: dict, settings: dict) -> None:
    document = json.loads(json.dumps(base))
    for key, value in settings.items():
        section, _, name = key.partition(".")
        document.setdefault(section, {})[name] = value
    folder = run_dir / USER_FOLDER
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "settings.json").write_text(json.dumps(document, indent=2))


# --- one run

def choose_extra(args, seed: int) -> tuple[dict, dict]:
    """The environment --mix and the settings --vary pick for one seed: the same seed, the same pick."""
    rng = random.Random(seed * 2654435761 + 0x5EED)
    env, settings = {}, {}
    if args.mix:
        area = rng.choice([None, None, "random", "1", "2", "3", "4", "5"])
        if area:
            env["KF_FUZZ_AREA"] = area
        if rng.random() < 0.5:
            env["KF_FUZZ_KIT"] = "1"
        if rng.random() < 0.5:
            env["KF_FUZZ_STRONG"] = "1"
        rate = rng.choice([None, "300", "1000"])
        if rate:
            env["KF_FUZZ_MACRORATE"] = rate
        if rng.random() < 0.2:
            env["KF_FUZZ_GODMODE"] = "1"
    if args.vary:
        for key, values in VARY.items():
            value = rng.choice(values)
            if value is not None:
                settings[key] = value
    return env, settings


def fuzz_env(args, run: Run, replay: Path | None = None) -> dict:
    env = {k: v for k, v in os.environ.items() if not k.startswith("KF_")}
    env.update({
        "XDG_DATA_HOME": str(run.dir / "home"),
        "KF_FUZZ_TICKS": str(args.ticks),
        "KF_FUZZ_STATUS": str(args.status),
        "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
        "ASAN_OPTIONS": "abort_on_error=0:detect_leaks=0",
    })
    if replay:
        env["KF_FUZZ_REPLAY"] = str(replay)
    else:
        env["KF_FUZZ"] = str(run.seed)
    switches = {"KF_FUZZ_STOP": args.stop, "KF_FUZZ_KIT": args.kit, "KF_FUZZ_STRONG": args.strong,
                "KF_FUZZ_GODMODE": args.godmode, "KF_FUZZ_LOAD": bool(args.load)}
    env.update({k: "1" for k, on in switches.items() if on})
    if args.no_macros:
        env["KF_FUZZ_MACROS"] = "0"
    values = {"KF_FUZZ_AREA": args.area, "KF_FUZZ_MACRORATE": args.macro_rate, "KF_FUZZ_WEIGHTS": args.weights,
              "KF_FUZZ_SKIP": args.skip, "KF_FUZZ_GAME_SEED": args.game_seed}
    env.update({k: str(v) for k, v in values.items() if v is not None})
    if args.trace:
        env["KF_TRACE"] = str(run.dir / "trace.txt")
    env.update(run.extra_env)
    for kv in args.env:
        k, _, v = kv.partition("=")
        env[k] = v
    return env


def prepare(args, run: Run, base_settings: dict) -> None:
    if run.dir.exists():
        shutil.rmtree(run.dir)
    (run.dir / "saves").mkdir(parents=True)
    if args.load:
        shutil.copy2(args.load, run.dir / "saves" / QUICK_CARD)
    write_settings(run.dir, base_settings, run.settings)


def launch(args, run: Run, env: dict) -> None:
    command = [str(binary(args)), "--data", str(args.data), "--saves", str(run.dir / "saves"), "--skip-intro"]
    if not args.windowed:
        command += ["--headless", "--turbo"]
    (run.dir / ENV_FILE).write_text(json.dumps({
        "seed": run.seed, "env": {k: v for k, v in env.items() if k.startswith("KF_")},
        "settings": run.settings, "command": command}, indent=2))
    log = open(run.dir / "log.txt", "w")
    run.proc = subprocess.Popen(command, cwd=run.dir, env=env, stdout=log, stderr=subprocess.STDOUT,
                                start_new_session=True)
    run.started = time.time()


def kill(run: Run, why: str) -> None:
    run.killed = why
    try:
        os.killpg(run.proc.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def poll(run: Run, timeout: float) -> bool:
    """True once the run is finished (exited or killed)."""
    if run.proc.poll() is not None:
        run.exit = run.proc.returncode
        run.seconds = time.time() - run.started
        return True
    if time.time() - run.started > timeout:
        kill(run, "timeout")
    return False


def crash_reports(run: Run) -> list[Path]:
    return sorted((run.dir / USER_FOLDER / "crashes").glob("crash-*/report.txt"))


def crash_signature(report: Path) -> tuple[str, str]:
    """(signature, headline) from a crash report: its fault and top frames past the plumbing."""
    text = report.read_text(errors="replace")
    head = next((line for line in text.splitlines() if line.startswith(("fault:", "terminate:"))), "crash")
    stack = text.split("== stack", 1)[1].split("== ", 1)[0] if "== stack" in text else ""
    frames = []
    for line in stack.splitlines():
        m = CRASH_FRAME_RE.match(line.strip())
        if m and not m.group(1).startswith(PLUMBING) and m.group(1) not in frames:
            frames.append(m.group(1).split("(")[0])
        if len(frames) == 4:
            break
    return " < ".join(frames) or head, head


def sanitizer_signature(log: str) -> tuple[str, str] | None:
    m = SANITIZER_RE.search(log)
    if not m:
        return None
    head = m.group(1)
    tail = log[m.start():]
    site = SANITIZER_SITE_RE.search(log)
    frame = next((f for f in SANITIZER_FRAME_RE.findall(tail) if not f.startswith(PLUMBING)), "")
    where = f"{Path(site.group(1)).name}:{site.group(2)}" if site and "runtime error" in head else frame
    kind = head.split(":")[1].strip() if head.startswith("ERROR") else "UBSan " + head.split(":", 1)[1].strip()[:80]
    return f"{kind} @ {where}", head[:200]


def classify(run: Run) -> None:
    log = (run.dir / "log.txt").read_text(errors="replace")
    events = [json.loads(m.group(1)) for line in log.splitlines() if (m := FUZZ_RE.match(line))]
    for event in events:
        if event.get("event") == "violation":
            run.findings.append({"kind": "violation", "signature": event["kind"],
                                 "headline": f"{event['kind']}: {event['detail']}", "update": event["update"]})
        elif event.get("event") == "end":
            run.end = event
    sanitizer = sanitizer_signature(log)
    asserted = ASSERT_RE.search(log)
    reports = crash_reports(run)
    if sanitizer:
        run.findings.append({"kind": "crash", "signature": sanitizer[0], "headline": sanitizer[1]})
    elif reports:
        sig, head = crash_signature(reports[0])
        if asserted:
            head = asserted.group(1)[:200]
        run.findings.append({"kind": "crash", "signature": sig, "headline": head, "report": rel(reports[0])})
    elif run.exit is not None and run.exit < 0 or (run.exit or 0) > 128:
        run.findings.append({"kind": "crash", "signature": f"signal {abs(run.exit) % 128}", "headline": "no report"})
    elif run.exit == 1 and not run.killed:
        message = [line for line in log.splitlines() if line and not line.startswith(("[", "Host:", "Starting", "Language"))]
        text = message[-1] if message else "exit 1"
        run.findings.append({"kind": "fail", "signature": text[:120], "headline": text[:200]})
    if run.killed:
        run.findings.append({"kind": "timeout", "signature": run.killed, "headline": run.killed})
    order = ("crash", "fail", "timeout", "violation")
    kinds = {f["kind"] for f in run.findings}
    run.outcome = next((k for k in order if k in kinds), "ok")


def clean(args, run: Run) -> None:
    if run.outcome != "ok" or args.keep_all:
        return
    for path in run.dir.iterdir():
        if path.name not in KEPT_ON_CLEAN:
            shutil.rmtree(path) if path.is_dir() else path.unlink()


def run_dir(args, seed: int, label: str = "") -> Path:
    return Path(args.out) / "runs" / (f"{seed:06d}{'-' + label if label else ''}")


def execute(args, runs: list[Run], base_settings: dict, timeout: float, replay: dict[int, Path] | None = None) -> None:
    """Runs them, args.jobs at a time; prints a line as each ends."""
    pending, active = list(runs), []
    total, done = len(runs), 0
    while pending or active:
        while pending and len(active) < args.jobs:
            run = pending.pop(0)
            prepare(args, run, base_settings)
            launch(args, run, fuzz_env(args, run, (replay or {}).get(id(run))))
            active.append(run)
        for run in [r for r in active if poll(r, timeout)]:
            active.remove(run)
            classify(run)
            clean(args, run)
            done += 1
            if not args.quiet:
                first = run.findings[0]["headline"] if run.findings else ""
                print(f"[{done}/{total}] seed {run.seed}{' ' + run.label if run.label else ''}: {run.outcome} "
                      f"{run.seconds:.0f}s {run.end.get('updates', '?')} updates  {first[:100]}", flush=True)
        time.sleep(0.05)


def seeds(args) -> list[int]:
    return list(range(args.seed_base, args.seed_base + args.runs))


def make_runs(args, label: str = "", settings: dict | None = None) -> list[Run]:
    out = []
    for seed in seeds(args):
        env, chosen = choose_extra(args, seed)
        chosen.update(dict(parse_setting(s) for s in args.setting))
        chosen.update(settings or {})
        out.append(Run(seed, run_dir(args, seed, label), extra_env=env, settings=chosen, label=label))
    return out


# --- the report

def rerun_command(args, run: Run) -> str:
    words = ["python3", "scripts/fuzz.py", "--runs", "1", "--seed-base", str(run.seed), "--ticks", str(args.ticks)]
    if args.preset != "linux":
        words += ["--preset", args.preset]
    for k, v in sorted(run.extra_env.items()):
        words += ["--env", f"{k}={v}"]
    for k, v in sorted(run.settings.items()):
        words += ["--setting", f"{k}={str(v).lower() if isinstance(v, bool) else v}"]
    return shlex.join(words)


def group(runs: list[Run]) -> dict[tuple[str, str], list[tuple[Run, dict]]]:
    groups: dict[tuple[str, str], list[tuple[Run, dict]]] = defaultdict(list)
    for run in runs:
        seen = set()
        for finding in run.findings:
            key = (finding["kind"], finding["signature"])
            if key not in seen:
                seen.add(key)
                groups[key].append((run, finding))
    return groups


def coverage(run_dirs: list[Path]) -> dict:
    floors, cells, items, runs = set(), set(), set(), 0
    for path in (d / "fuzz-coverage.txt" for d in sorted(run_dirs)):
        if not path.exists():
            continue
        runs += 1
        for line in path.read_text().splitlines():
            words = line.split()
            if words[:1] == ["floor"]:
                floors.add(int(words[1]))
            elif words[:1] == ["cell"]:
                cells.add(tuple(map(int, words[1:4])))
            elif words[:1] == ["item"]:
                items.add(int(words[1]))
    per_floor = Counter(c[0] for c in cells)
    return {"runs": runs, "floors": sorted(floors), "cells_per_floor": {str(k): per_floor[k] for k in sorted(per_floor)},
            "cells": len(cells), "items": sorted(items)}


def coverage_markdown(cov: dict) -> list[str]:
    lines = ["## Coverage", "",
             f"{cov['runs']} runs reached floors {cov['floors']}, {cov['cells']} cells "
             f"({', '.join(f'floor {k}: {v}' for k, v in cov['cells_per_floor'].items())}), "
             f"and held {len(cov['items'])} kinds of item.", ""]
    return lines


def write_report(args, runs: list[Run], wall: float) -> None:
    out = Path(args.out)
    counts = Counter(r.outcome for r in runs)
    groups = group(runs)
    cov = coverage([r.dir for r in runs])
    md = ["# Fuzz report", "",
          f"{len(runs)} runs of {args.ticks} ticks ({args.ticks / TICKS_PER_SECOND / 60:.1f} game minutes each) "
          f"in {wall:.0f} s, {args.jobs} at a time, `{rel(binary(args))}`, seeds "
          f"{min(r.seed for r in runs)}-{max(r.seed for r in runs)}.", "",
          " | ".join(f"**{k}** {counts[k]}" for k in ("crash", "fail", "timeout", "violation", "ok") if counts[k]), ""]
    for (kind, sig), hits in sorted(groups.items(), key=lambda kv: (-len(kv[1]), kv[0])):
        run, finding = hits[0]
        md += [f"## {kind}: {sig}", "", f"- {finding['headline']}",
               f"- {len(hits)} run(s): seeds {', '.join(str(r.seed) for r, _ in hits[:20])}",
               f"- example: `{rel(run.dir)}`" + (f", report `{finding['report']}`" if "report" in finding else ""),
               f"- rerun: `{rerun_command(args, run)}`",
               f"- replay: `python3 scripts/fuzz.py --replay {rel(run.dir)}`"]
        if args.vary or args.mix:
            settings = Counter(f"{k}={v}" for r, _ in hits for k, v in {**r.settings, **r.extra_env}.items())
            md.append("- set in its runs: " + ", ".join(f"{k} ({n}/{len(hits)})" for k, n in settings.most_common(8)))
        md.append("")
    md += coverage_markdown(cov)
    (out / "report.md").write_text("\n".join(md) + "\n")
    (out / "report.json").write_text(json.dumps({
        "runs": [{"seed": r.seed, "label": r.label, "dir": rel(r.dir), "outcome": r.outcome, "exit": r.exit,
                  "seconds": round(r.seconds, 1), "findings": r.findings, "end": r.end,
                  "env": r.extra_env, "settings": r.settings} for r in runs],
        "groups": [{"kind": k, "signature": s, "seeds": [r.seed for r, _ in hits]} for (k, s), hits in groups.items()],
        "coverage": cov}, indent=2))
    print(f"\n{len(runs)} runs in {wall:.0f} s: " + ", ".join(f"{k} {n}" for k, n in counts.most_common()))
    for (kind, sig), hits in sorted(groups.items(), key=lambda kv: -len(kv[1]))[:12]:
        print(f"  {len(hits):3d}  {kind}: {sig}")
    print(f"report: {rel(out / 'report.md')}")


# --- the modes

def compare_traces(args, a: Path, b: Path) -> dict:
    result = subprocess.run([str(binary(args)), "--compare-traces", str(a), str(b)], capture_output=True, text=True)
    try:
        return json.loads(result.stdout.strip().splitlines()[-1])
    except (json.JSONDecodeError, IndexError):
        return {"ok": False, "error": result.stdout + result.stderr}


def paired(args, base_settings: dict, label_b: str, settings_b: dict) -> int:
    """Each seed twice, the second with settings_b; their state traces must agree."""
    args.trace = True
    first, second = make_runs(args, "a"), make_runs(args, label_b, settings_b)
    started = time.time()
    execute(args, first + second, base_settings, args.timeout)
    differing = 0
    lines = [f"# {label_b}: {json.dumps(settings_b) if settings_b else 'the same run twice'}", ""]
    for a, b in zip(first, second):
        result = compare_traces(args, a.dir / "trace.txt", b.dir / "trace.txt")
        if not result.get("ok"):
            line = f"seed {a.seed}: cannot compare ({result.get('error', '')[:120]})"
            differing += 1
        elif result["equal"]:
            line = f"seed {a.seed}: equal for {result['updates']} updates"
        else:
            differing += 1
            line = (f"seed {a.seed}: **differ** at update {result['update']} in `{result['region']}` "
                    f"(ticks {result['tickA']} and {result['tickB']}; `{rel(a.dir)}` against `{rel(b.dir)}`)")
        print(line)
        lines.append(f"- {line}")
    Path(args.out).mkdir(parents=True, exist_ok=True)
    (Path(args.out) / f"compare-{label_b}.md").write_text("\n".join(lines) + "\n")
    print(f"{len(first) - differing} of {len(first)} seeds agree ({time.time() - started:.0f} s); "
          f"{rel(Path(args.out) / f'compare-{label_b}.md')}")
    return 1 if differing else 0


def replay(args, base_settings: dict) -> int:
    source = Path(args.replay)
    saved = json.loads((source / ENV_FILE).read_text()) if (source / ENV_FILE).exists() else {}
    run = Run(saved.get("seed", 0), Path(args.out) / "replays" / source.name, label="replay",
              settings=saved.get("settings", {}))
    run.extra_env = {k: v for k, v in saved.get("env", {}).items()
                     if k.startswith("KF_FUZZ_") and k not in ("KF_FUZZ_REPLAY", "KF_FUZZ_TICKS", "KF_FUZZ_STATUS")}
    if "KF_FUZZ_TICKS" in saved.get("env", {}):
        args.ticks = int(saved["env"]["KF_FUZZ_TICKS"])
    recording = (source / "fuzz-input.log").resolve()
    if args.windowed:
        args.jobs = 1
    execute(args, [run], base_settings, args.timeout, replay={id(run): recording})
    print(f"{run.outcome}: " + "; ".join(f["headline"] for f in run.findings[:5]) + f"\n{rel(run.dir)}")
    return 0 if run.outcome == "ok" else 1


def read_recording(path: Path) -> tuple[list[str], list[str]]:
    header, events = [], []
    for line in path.read_text().splitlines():
        (events if line[:2] in ("b ", "m ") else header).append(line)
    return header, events


def minimise(args, base_settings: dict) -> int:
    """Delta debugging over a recording's lines: the fewest that still give the same finding."""
    source = Path(args.minimise)
    saved = json.loads((source / ENV_FILE).read_text())
    target = Run(saved["seed"], source)
    classify(target)
    if not target.findings:
        print(f"{rel(source)} found nothing to keep")
        return 1
    want = (target.findings[0]["kind"], target.findings[0]["signature"])
    print(f"keeping: {want[0]}: {want[1]}")
    header, events = read_recording(source / "fuzz-input.log")
    work = Path(args.out) / "minimise" / source.name
    work.mkdir(parents=True, exist_ok=True)
    tries = 0

    def still_breaks(candidate: list[str]) -> bool:
        nonlocal tries
        tries += 1
        recording = work / f"try-{tries}.log"
        recording.write_text("\n".join(header + candidate) + "\n")
        run = Run(saved["seed"], work / f"run-{tries}", label="min", settings=saved.get("settings", {}),
                  extra_env={k: v for k, v in saved["env"].items() if k.startswith("KF_FUZZ_") and k != "KF_FUZZ"})
        args.quiet = True
        execute(args, [run], base_settings, args.timeout, replay={id(run): recording})
        hit = any((f["kind"], f["signature"]) == want for f in run.findings)
        shutil.rmtree(run.dir, ignore_errors=True)
        return hit

    chunks = 2
    while len(events) >= 2 and tries < args.max_tries:
        size = max(len(events) // chunks, 1)
        for start in range(0, len(events), size):
            candidate = events[:start] + events[start + size:]
            if still_breaks(candidate):
                events, chunks = candidate, max(chunks - 1, 2)
                print(f"  {len(events)} lines ({tries} tries)", flush=True)
                break
        else:
            if size == 1:
                break
            chunks = min(chunks * 2, len(events))
    final = work / "fuzz-input.log"
    final.write_text("\n".join(header + events) + "\n")
    (work / ENV_FILE).write_text(json.dumps(saved, indent=2))
    print(f"{len(events)} lines after {tries} tries: {rel(final)}\n"
          f"replay: python3 scripts/fuzz.py --replay {rel(work)}")
    return 0


def campaign(args, base_settings: dict) -> int:
    started = time.time()
    if args.budget:
        runs, seed = [], args.seed_base
        while time.time() - started < args.budget:
            args.runs, args.seed_base = args.jobs, seed
            batch = make_runs(args)
            execute(args, batch, base_settings, args.timeout)
            runs += batch
            seed += args.jobs
    else:
        runs = make_runs(args)
        execute(args, runs, base_settings, args.timeout)
    write_report(args, runs, time.time() - started)
    return 1 if any(r.outcome in ("crash", "fail") for r in runs) else 0


# --- libFuzzer targets (the fuzz preset): what comes from outside the game

# name: (what it reads, the files that seed its corpus: globs under the repo, the user folder
# and the earlier C# build beside this repo; local only, the corpus is gitignored).
TARGETS = {
    "memory_card": ("a raw 128 KB memory card image", ["../verdite1/*.sav", "../verdite1/**/*.mcd", "../verdite1/**/*.mcr"]),
    "save_card": ("the game's 40960-byte save card, as the game file and the quick save",
                  ["fuzz/**/saves/BISLPS-00017KF*", "~/.local/share/Verdite/Verdite1/BISLPS-00017KF*"]),
    "disc_image": ("a disc image, plain or CHD", ["libverdite/tests/data/disc/*.bin", "libverdite/tests/data/disc/*.chd",
                                                   "libverdite/tests/data/disc/*.iso"]),
    "codecs": ("the decomp's codecs: TIM, assets, placements, audio banks, music", ["data/en/**/*.TIM"]),
}
FINDING_PREFIXES = ("crash-", "leak-", "timeout-", "oom-", "slow-unit-")


def seed_corpus(name: str, corpus: Path, extra: list[str]) -> int:
    added = 0
    for pattern in TARGETS[name][1] + extra:
        expanded = Path(pattern).expanduser()
        base, glob = (Path("/"), str(expanded)[1:]) if expanded.is_absolute() else (REPO, pattern)
        matches = [expanded] if expanded.is_file() else list(base.glob(glob))
        for path in matches[:64]:
            if path.is_file() and path.stat().st_size <= 8 * 1024 * 1024:
                target = corpus / f"seed-{added:03d}-{path.name}"
                if not target.exists():
                    shutil.copy2(path, target)
                    added += 1
    return added


def targets(args) -> int:
    names = args.targets or list(TARGETS)
    unknown = [n for n in names if n not in TARGETS]
    if unknown:
        print(f"fuzz: no target {', '.join(unknown)}; there are {', '.join(TARGETS)}", file=sys.stderr)
        return 2
    build = binary(args).parent
    procs = []
    for name in names:
        exe = build / f"fuzz_{name}"
        if not exe.exists():
            print(f"fuzz: {rel(exe)} is missing; nix develop -c cmake --build --preset fuzz", file=sys.stderr)
            return 2
        work = Path(args.out) / "targets" / name
        corpus = Path(args.out) / "corpus" / name
        corpus.mkdir(parents=True, exist_ok=True)
        work.mkdir(parents=True, exist_ok=True)
        seeded = seed_corpus(name, corpus, args.corpus_from)
        command = [str(exe), str(corpus), f"-max_total_time={int(args.seconds)}", f"-artifact_prefix={work}/",
                   "-rss_limit_mb=4096", "-print_final_stats=1"]
        if args.max_len:
            command.append(f"-max_len={args.max_len}")
        log = open(work / "log.txt", "w")
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        procs.append((name, exe, work, corpus, seeded,
                      subprocess.Popen(command, cwd=work, stdout=log, stderr=subprocess.STDOUT, env=env)))
        print(f"{name}: {TARGETS[name][0]}; {seeded} new seed file(s), {len(list(corpus.iterdir()))} in the corpus")
    worst = 0
    for name, exe, work, corpus, _, proc in procs:
        proc.wait()
        text = (work / "log.txt").read_text(errors="replace")
        stats = re.findall(r"^#\d+\s+DONE\s+(.*)$", text, re.M)
        findings = sorted(p for p in work.iterdir() if p.name.startswith(FINDING_PREFIXES))
        head = SANITIZER_RE.search(text) or ASSERT_RE.search(text)
        print(f"\n{name}: exit {proc.returncode}; {stats[-1] if stats else 'no final stats'}")
        if head:
            print(f"  {head.group(1)[:160]}")
        for finding in findings:
            print(f"  {rel(finding)}\n    again: {rel(exe)} {rel(finding)}")
        if findings or proc.returncode not in (0,):
            worst = 1
    return worst


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0], formatter_class=argparse.RawDescriptionHelpFormatter,
                                epilog="The KF_FUZZ_* switches these set are in port/include/kf/platform/fuzz.h.")
    g = p.add_argument_group("what to run")
    g.add_argument("--runs", type=int, default=12, help="seeds to run (default 12)")
    g.add_argument("--seed-base", type=int, help="the first seed (default: a new random one each time, printed)")
    g.add_argument("--budget", type=float, help="launch seeds for this many seconds instead of --runs")
    g.add_argument("--jobs", type=int, default=max((os.cpu_count() or 2) // 2, 1), help="runs at once (default: half the CPUs)")
    g.add_argument("--ticks", type=int, default=72000, help="game-clock ticks per run, 60 a second (default 72000: 20 minutes)")
    g.add_argument("--timeout", type=float, default=900, help="wall-clock seconds before a run is killed (default 900)")
    g.add_argument("--preset", default="linux", help="the build: linux, sanitize or fuzz (build/<preset>/verdite1)")
    g.add_argument("--binary", help="a binary other than build/<preset>/verdite1")
    g.add_argument("--data", default=str(DATA), help="the game's extracted files (default data/en)")
    g.add_argument("--out", default=str(REPO / "fuzz"), help="where runs and the report go (default fuzz/)")
    g = p.add_argument_group("how a run plays")
    g.add_argument("--stop", action="store_true", help="end a run at its first violation")
    g.add_argument("--no-macros", action="store_true", help="no warps, gotos, saves or loads: the pad only")
    g.add_argument("--macro-rate", type=int, help="every macro's chance, in percent (default 100)")
    g.add_argument("--weights", help="name=weight,...: a press's weight or a macro's chance per 10000 updates")
    g.add_argument("--skip", help="invariants to turn off: vitals,progress,position,cell,in-rock,items,equipment,actors,effects")
    g.add_argument("--area", help="start on floor 1-5, or random")
    g.add_argument("--kit", action="store_true", help="start with every item and spells 0-8")
    g.add_argument("--strong", action="store_true", help="start with max HP and MP 999")
    g.add_argument("--godmode", action="store_true", help="HP topped up every update")
    g.add_argument("--load", metavar="CARD", help=f"start by quick loading this card file (a {QUICK_CARD})")
    g.add_argument("--game-seed", type=int, help="the game's rand seed (default: the run's seed)")
    g.add_argument("--status", type=int, default=3600, help="ticks between status lines (default 3600)")
    g.add_argument("--setting", action="append", default=[], metavar="SECTION.KEY=VALUE",
                   help="a settings.json value for every run, e.g. gameplay.classic_mode=true (repeatable)")
    g.add_argument("--settings", metavar="FILE", help="a settings.json to start each run's from")
    g.add_argument("--env", action="append", default=[], metavar="KEY=VALUE", help="extra environment (repeatable)")
    g.add_argument("--mix", action="store_true", help="each seed picks its start: floor, kit, strength, macro rate, godmode")
    g.add_argument("--vary", action="store_true", help="each seed picks gameplay and display settings (VARY in this script)")
    g.add_argument("--trace", action="store_true", help="write each run's state trace (trace.txt)")
    g = p.add_argument_group("modes")
    g.add_argument("--replay", metavar="RUN_DIR", help="press a run's recording back, with its switches and settings")
    g.add_argument("--windowed", action="store_true", help="with --replay: in a window at normal speed, to watch")
    g.add_argument("--check-determinism", action="store_true", help="each seed twice; the state traces must agree")
    g.add_argument("--compare-setting", nargs="?", const="", metavar="SECTION.KEY=VALUE",
                   help="each seed with and without a setting; the traces must agree. Alone: every visual setting")
    g.add_argument("--minimise", metavar="RUN_DIR", help="cut a run's recording to the fewest lines that keep its finding")
    g.add_argument("--max-tries", type=int, default=200, help="replays --minimise may spend (default 200)")
    g.add_argument("--coverage-only", metavar="DIR", help="print a fuzz directory's merged coverage and stop")
    g.add_argument("--targets", nargs="*", metavar="NAME",
                   help=f"run the libFuzzer targets instead of the game (fuzz preset): {', '.join(TARGETS)}; none named: all")
    g.add_argument("--seconds", type=float, default=300, help="with --targets: how long each runs (default 300)")
    g.add_argument("--max-len", type=int, help="with --targets: the longest input libFuzzer makes")
    g.add_argument("--corpus-from", action="append", default=[], metavar="PATH",
                   help="with --targets: more files (or globs) to seed the corpus with (repeatable)")
    g = p.add_argument_group("output")
    g.add_argument("--keep-all", action="store_true", help="keep every file of clean runs too")
    g.add_argument("--quiet", action="store_true", help="no line per run")
    args = p.parse_args()

    # Each run's working directory is its own, so every path it is handed must be absolute.
    args.out, args.data = str(Path(args.out).resolve()), str(Path(args.data).resolve())
    if args.load:
        args.load = str(Path(args.load).resolve())
    if args.coverage_only:
        print("\n".join(coverage_markdown(coverage(list((Path(args.coverage_only) / "runs").iterdir())))))
        return 0
    if args.targets is not None:
        if args.preset == "linux":
            args.preset = "fuzz"
        return targets(args)
    if problem := stale_build(args):
        print(f"fuzz: {problem}", file=sys.stderr)
        return 2
    if not Path(args.data).is_dir():
        print(f"fuzz: no game data at {args.data}", file=sys.stderr)
        return 2
    if args.windowed and not args.replay:
        print("fuzz: --windowed is for --replay", file=sys.stderr)
        return 2
    if args.preset == "sanitize":
        args.timeout = max(args.timeout, 3600)
    if args.seed_base is None:
        # A fixed base would play the same seeds, and so the same --mix and --vary picks, every time.
        args.seed_base = random.SystemRandom().randrange(1, 1_000_000_000)
        if not args.replay and not args.minimise:
            print(f"fuzz: seeds from {args.seed_base} (--seed-base {args.seed_base} plays them again)")
    base_settings = json.loads(Path(args.settings).read_text()) if args.settings else {}
    Path(args.out).mkdir(parents=True, exist_ok=True)

    if args.replay:
        return replay(args, base_settings)
    if args.minimise:
        return minimise(args, base_settings)
    if args.check_determinism:
        return paired(args, base_settings, "again", {})
    if args.compare_setting is not None:
        items = [args.compare_setting] if args.compare_setting else list(VISUAL_SETTINGS)
        worst = 0
        for item in items:
            key, value = parse_setting(item)
            worst = max(worst, paired(args, base_settings, re.sub(r"[^\w.-]", "_", item), {key: value}))
        return worst
    return campaign(args, base_settings)


if __name__ == "__main__":
    sys.exit(main())
