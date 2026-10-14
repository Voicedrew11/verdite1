#!/usr/bin/env python3
"""Summarise a frame profile: where the frame time goes, and the worst frames.

Reads the CSV that KF1_PROFILE_OUT writes (or the profiler panel's Save CSV):
one row per section per frame, plus frame.total / frame.gc_pause / frame.alloc_kb /
frame.jit pseudo-rows, and gpu.* rows (GPU ms per present, runtime 0084). See
"Profiling a frame" in docs/DEVELOPMENT.md.

    python3 scripts/profile_report.py profile.csv
    python3 scripts/profile_report.py profile.csv --skip 5 --worst 15 --top 25
    python3 scripts/profile_report.py profile.csv --match func_800422B8

--skip drops the first N seconds (boot and first-hit JIT); --from/--to pick a
time range in seconds from the first frame.
"""
import argparse
import csv
import statistics
import sys
from collections import defaultdict


def pct(sorted_values, p):
    if not sorted_values:
        return 0.0
    return sorted_values[min(len(sorted_values) - 1, int(len(sorted_values) * p))]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv")
    ap.add_argument("--skip", type=float, default=0.0, help="seconds to drop from the start")
    ap.add_argument("--from", dest="t0", type=float, default=None, help="start, seconds from the first frame")
    ap.add_argument("--to", dest="t1", type=float, default=None, help="end, seconds from the first frame")
    ap.add_argument("--top", type=int, default=20, help="sections to list")
    ap.add_argument("--worst", type=int, default=10, help="worst frames (by work) to break down")
    ap.add_argument("--waits", action="store_true", help="list Wait sections in the ranking too")
    ap.add_argument("--match", default=None, help="only list sections whose name contains this")
    args = ap.parse_args()

    frames = {}                              # index -> dict
    first_t = None
    with open(args.csv, newline="") as fh:
        for r in csv.DictReader(fh):
            if not r.get("self_ms"):
                continue                     # a row cut short by the process ending
            idx = int(r["frame"])
            t = float(r["time_ms"])
            if first_t is None or t < first_t:
                first_t = t
            f = frames.get(idx)
            if f is None:
                f = frames[idx] = {"index": idx, "t": t, "total": 0.0, "gc": 0.0, "alloc": 0.0, "jit": 0.0,
                                   "wait": 0.0, "gpu": 0.0, "sections": []}
            name, group, self_ms = r["section"], r["group"], float(r["self_ms"])
            if group == "Frame":
                key = {"frame.total": "total", "frame.gc_pause": "gc",
                       "frame.alloc_kb": "alloc", "frame.jit": "jit"}.get(name)
                if key:
                    f[key] = self_ms
                elif name.startswith("gpu."):
                    f.setdefault("gputime", {})[name[4:]] = self_ms
                continue
            incl = float(r["incl_ms"]) if r["incl_ms"] else 0.0
            calls = int(r["calls"]) if r["calls"] else 0
            f["sections"].append((name, group, self_ms, incl, calls))
            if group == "Wait":
                f["wait"] += self_ms
            elif group == "Gpu":
                f["gpu"] += self_ms

    if not frames:
        sys.exit("no frames in " + args.csv)

    t0 = args.t0 if args.t0 is not None else args.skip
    t1 = args.t1 if args.t1 is not None else float("inf")
    chosen = [f for f in (frames[k] for k in sorted(frames))
              if t0 <= (f["t"] - first_t) / 1000.0 <= t1]
    if not chosen:
        sys.exit("no frames in that range")

    n = len(chosen)
    span_s = (chosen[-1]["t"] + chosen[-1]["total"] - chosen[0]["t"]) / 1000.0
    totals = sorted(f["total"] for f in chosen)
    for f in chosen:
        f["work"] = f["total"] - f["wait"] - f["gpu"]
    works = sorted(f["work"] for f in chosen)

    print(f"{n} frames over {span_s:.1f} s ({n / span_s if span_s else 0:.1f} fps)")
    print(f"frame  avg {statistics.fmean(totals):7.3f}  median {pct(totals, .5):7.3f}  "
          f"p99 {pct(totals, .99):7.3f}  max {totals[-1]:7.3f} ms")
    print(f"work   avg {statistics.fmean(works):7.3f}  median {pct(works, .5):7.3f}  "
          f"p99 {pct(works, .99):7.3f}  max {works[-1]:7.3f} ms")
    print(f"swap   avg {statistics.fmean(f['gpu'] for f in chosen):7.3f} ms   "
          f"wait avg {statistics.fmean(f['wait'] for f in chosen):7.3f} ms")
    gpu_frames = [f["gputime"] for f in chosen if "gputime" in f]
    if gpu_frames:
        passes = [k for k in gpu_frames[0] if k != "total"]
        print(f"GPU    avg {statistics.fmean(g.get('total', 0.0) for g in gpu_frames):7.3f} ms/present: " +
              ", ".join(f"{k} {statistics.fmean(g.get(k, 0.0) for g in gpu_frames):.3f}" for k in passes))
    print(f"GC {sum(f['gc'] for f in chosen):.1f} ms paused, JIT {sum(f['jit'] for f in chosen):.1f} ms, "
          f"{statistics.fmean(f['alloc'] for f in chosen):.1f} KB/frame allocated")

    agg = defaultdict(lambda: [0.0, 0.0, 0.0, 0, None])     # self, max self, incl, calls, group
    for f in chosen:
        for name, group, s, incl, calls in f["sections"]:
            a = agg[name]
            a[0] += s
            a[1] = max(a[1], s)
            a[2] += incl
            a[3] += calls
            a[4] = group
    work_total = sum(a[0] for a in agg.values() if a[4] not in ("Wait", "Gpu")) or 1.0

    rows = [(name, a) for name, a in agg.items()
            if (args.waits or a[4] != "Wait") and (args.match is None or args.match in name)]
    rows.sort(key=lambda kv: -kv[1][0])
    print(f"\n{'self ms/fr':>10} {'max self':>9} {'incl ms/fr':>10} {'calls/fr':>9} {'% work':>7}  group    section")
    for name, (s, mx, incl, calls, group) in rows[:args.top]:
        share = f"{100 * s / work_total:6.1f}%" if group not in ("Wait", "Gpu") else "      -"
        print(f"{s / n:10.3f} {mx:9.3f} {incl / n:10.3f} {calls / n:9.1f} {share}  {group:<8} {name}")

    if args.worst > 0:
        print(f"\nworst {args.worst} frames by work:")
        for f in sorted(chosen, key=lambda f: -f["work"])[:args.worst]:
            idx = f["index"]
            extra = (f"  GC {f['gc']:.2f}" if f["gc"] else "") + (f"  JIT {f['jit']:.2f}" if f["jit"] > 0.05 else "")
            top = sorted((s for s in f["sections"] if s[1] != "Wait"), key=lambda s: -s[2])[:5]
            print(f"  frame {idx:>7} @ {(f['t'] - first_t) / 1000.0:7.2f} s  work {f['work']:8.2f} of {f['total']:8.2f} ms{extra}")
            for name, group, s, _, calls in top:
                print(f"      {s:8.2f}  {name}" + (f"  x{calls}" if calls > 1 else ""))


if __name__ == "__main__":
    main()
