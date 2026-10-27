#!/usr/bin/env python3
"""The floor handover's round trip (docs/COOP_PLAN.md, "The handover is live"): for each seed and
floor, two seeded headless runs play the same; in the second, at one update, the floor is captured,
unloaded, loaded from its record and the snapshot put back, as a machine taking the floor does.
The floor's state must then step update for update as in the first (KF_FLOOR_TRACE, compared with
--compare-traces). Each run has its own saves and user folder.

    scripts/floor_handover.py build/linux/verdite1 [--seeds 1,2] [--floors 1,2,3,4,5]
        [--at 1500] [--ticks 16000] [--work DIR]
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def run(binary, home, seed, floor, ticks, at):
    (home / "saves").mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, XDG_DATA_HOME=str(home), KF_FUZZ=str(seed), KF_FUZZ_MACROS="0",
               KF_FUZZ_AREA=str(floor), KF_FUZZ_GODMODE="1", KF_FUZZ_TICKS=str(ticks),
               KF_FUZZ_COVERAGE="0", KF_FLOOR_TRACE=str(home / "floor.txt"),
               UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1", ASAN_OPTIONS="detect_leaks=0")
    if at:
        env["KF_FLOOR_HANDOVER"] = str(at)
    with open(home / "log.txt", "w") as log:
        code = subprocess.run([binary, "--data", str(ROOT / "data/en"), "--saves", str(home / "saves"),
                               "--skip-intro", "--headless", "--turbo"], cwd=home, env=env, stdout=log,
                              stderr=subprocess.STDOUT, timeout=900).returncode
    handover = [line.strip() for line in open(home / "log.txt", errors="replace") if "floor handover" in line]
    return code, handover


def check(binary, work, seed, floor, ticks, at):
    case = work / f"seed{seed}-floor{floor}"
    plain = run(binary, case / "plain", seed, floor, ticks, 0)
    handed = run(binary, case / "handed", seed, floor, ticks, at)
    name = f"seed {seed}, floor {floor}"
    if plain[0] != 0 or handed[0] != 0:
        return False, f"{name}: exit {plain[0]} and {handed[0]} ({case})"
    if not handed[1] or "refused" in handed[1][0]:
        return False, f"{name}: {handed[1][0] if handed[1] else 'no handover (the run ended first)'}"
    compared = subprocess.run([binary, "--compare-traces", str(case / "plain/floor.txt"),
                               str(case / "handed/floor.txt")], capture_output=True, text=True)
    result = json.loads(compared.stdout.strip().splitlines()[-1])
    detail = handed[1][0].removeprefix("[KF1] ")
    if not result.get("equal"):
        return False, f"{name}: parts at update {result.get('update')} in {result.get('region')} ({detail})"
    if result["updates"] <= at:
        return False, f"{name}: the plain run ended at update {result['updates']}, before the handover"
    return True, f"{name}: equal for {result['updates'] - at} updates after it ({detail})"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("binary")
    parser.add_argument("--seeds", default="1,2")
    parser.add_argument("--floors", default="1,2,3,4,5")
    parser.add_argument("--at", type=int, default=1500, help="the update of the handover")
    parser.add_argument("--ticks", type=int, default=16000, help="game-clock ticks of each run")
    parser.add_argument("--work", help="where the runs go (default: a temporary folder, kept on failure)")
    args = parser.parse_args()
    if not (ROOT / "data/en").is_dir():
        print("floor_handover: skipped, no data/en")
        return 0
    work = Path(args.work) if args.work else Path(tempfile.mkdtemp(prefix="floor-handover-"))
    cases = [(int(seed), int(floor)) for seed in args.seeds.split(",") for floor in args.floors.split(",")]
    binary = str(Path(args.binary).resolve())
    with ThreadPoolExecutor(max_workers=min(len(cases), os.cpu_count() or 4)) as pool:
        results = list(pool.map(lambda case: check(binary, work, *case, args.ticks, args.at), cases))
    for passed, line in results:
        print(("ok    " if passed else "FAIL  ") + line)
    failed = sum(not passed for passed, _ in results)
    print(f"floor_handover: {len(results) - failed} of {len(results)} equal ({work})")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
