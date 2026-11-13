#!/usr/bin/env python3
"""
Compile-Performance Budget Gate (Sprint 178, dev-plan Verification &
Scale item 3)

Ratcheted compile-time budget for zerocc over a fixed corpus of
increasing size (the four bench workloads, the temp-id churn stress,
cJSON, zlib deflate.c, the 255k-line sqlite3.c amalgamation, and a
pinned 20-file c-torture sample that directly guards the c-torture
sweep's wall time). Per case and `-O` level the gate measures:

  * WALL TIME (whole compile, `-S` path: parse -> lower -> optimize ->
    asm emit; no assembler/linker). Noisy: the OKF
    perf-measurements.md methodology notes host wall times swing
    +-50% under variable load, so the wall budget carries a generous
    50% tolerance.
  * PER-PASS OPTIMIZER TIME (in-process CLOCK_MONOTONIC via
    `--profile-opts`, unaffected by page-cache/linker variance) for
    `-O>=1`. This is the stable metric and carries a lenient tolerance,
    max(40% relative, 3ms absolute): per-pass times on the large single-
    function workloads (churn, sqlite3) jitter +-20% run-to-run even
    in-process (measured: GVN on churn spans 6.5-10.6ms across 12
    runs), so the floor must absorb median-of-N sampling noise while
    still catching 2x pass regressions. The per-pass table is the
    continuation of the docs/okf/perf-measurements.md baseline
    (mem2reg/GVN fixed in Sprints 36/38; LICM is the current dominant
    pass).

Gate semantics (baseline ratchet, test/perf/baseline.json):
  * a case/level missing from the baseline FAILs (new workloads must be
    ratcheted with --update-baseline)
  * a measured value above budget x (1 + tolerance) FAILs
  * a measured pass present in the run but not in the baseline FAILs
    (a new pass must be ratcheted)
  * a baseline pass absent from the run is treated as 0 (removed pass
    = improvement)
  * improvements are reported informationally (re-record with
    --update-baseline to pull the budget down)

Usage:
  python3 tools/verify/perf_budget.py                 # CI sweep (5 runs, sqlite3 3)
  python3 tools/verify/perf_budget.py --verbose
  python3 tools/verify/perf_budget.py --levels 0 2 3
  python3 tools/verify/perf_budget.py --only sqlite3 --runs 7
  python3 tools/verify/perf_budget.py --update-baseline
"""

import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from tools import find_repo_root

ROOT = find_repo_root()
ZC = os.path.join(str(ROOT), "build", "bin", "zerocc")
TORTURE_DIR = os.path.join(str(ROOT), "third_party", "vendor", "c-torture")
PERF_DIR = os.path.join(str(ROOT), "test", "perf")
BASELINE = os.path.join(PERF_DIR, "baseline.json")

TOL_WALL = 0.50   # host wall times swing +-50% under load (OKF methodology)
TOL_PASS = 0.40   # in-process per-pass: lenient - large single-function
PASS_FLOOR = 3.0  # workloads jitter +-20% (churn GVN: 6.5-10.6ms over 12 runs)

# fixed corpus, increasing size. The torture_sample aggregate (pinned
# list) guards the c-torture sweep's per-test fixed cost.
CASES = {
    "bench_arith":   {"file": "test/bench/arith_heavy.c",       "runs": 5},
    "bench_call":    {"file": "test/bench/call_heavy.c",        "runs": 5},
    "bench_loop":    {"file": "test/bench/loop_heavy.c",        "runs": 5},
    "bench_memory":  {"file": "test/bench/memory_heavy.c",      "runs": 5},
    "bench_synth":   {"file": "test/bench/synth_large.c",       "runs": 5},
    "churn":         {"file": "test/jit/temp_renumber_churn.c", "runs": 7},
    "cjson":         {"file": "test/projects/cjson/cJSON.c",    "runs": 5},
    "deflate":       {"file": "test/projects/zlib/deflate.c",   "runs": 5},
    "sqlite3":       {"file": "test/projects/sqlite/sqlite3.c", "runs": 7},
    "torture_sample": {"torture": True, "runs": 3},
}

PASS_RE = re.compile(r"^  (\S.*?)\s{2,}([0-9]+\.?[0-9]*)\s*$")


def torture_files():
    ex = os.path.join(TORTURE_DIR, "execute")
    names = sorted(n for n in os.listdir(ex) if n.endswith(".c"))
    return [os.path.join("execute", n) for n in names[:20]]


def parse_passes(stderr_text):
    """Parse the `--profile-opts` Per-pass section (stderr) into
    {name: ms}. None when the section is absent (-O0 runs no passes)."""
    passes = {}
    in_sec = False
    for line in stderr_text.splitlines():
        if line.startswith("Per-pass wall time"):
            in_sec = True
            continue
        if in_sec:
            if line.startswith("General:") or (line.strip() == "" and passes):
                break
            m = PASS_RE.match(line)
            if m:
                passes[m.group(1)] = float(m.group(2))
    return passes or None


def merge_passes(a, b):
    if a is None:
        return b
    if b is None:
        return a
    for k, v in b.items():
        a[k] = a.get(k, 0.0) + v
    return a


def measure_case(name, spec, level, runs, tmp):
    profile = level >= 1
    if spec.get("torture"):
        files = [os.path.join(TORTURE_DIR, f) for f in torture_files()]
        missing = [f for f in files if not os.path.exists(f)]
        if missing:
            raise RuntimeError("torture sample files missing: %s" %
                               ", ".join(missing[:3]))
        target = files
    else:
        f = os.path.join(ROOT, spec["file"])
        if not os.path.exists(f):
            raise RuntimeError("corpus file missing: %s" % f)
        target = [f]
    # warm run (page cache), discarded
    wall, _ = timed_compile(target, level, profile, tmp)
    walls, passsets = [], []
    for _ in range(runs):
        w, pp = timed_compile(target, level, profile, tmp)
        walls.append(w)
        if pp:
            passsets.append(pp)
    med_wall = statistics.median(walls)
    med_pass = None
    if passsets:
        med_pass = {}
        for k in passsets[0]:
            med_pass[k] = statistics.median([p.get(k, 0.0) for p in passsets])
    return med_wall, med_pass


def timed_compile(files, level, profile, tmp):
    """Timed single measurement; files==[f] or the torture sample list.
    Returns (wall_s, passes_or_None)."""
    out = os.path.join(tmp, "out.s")
    t0 = time.perf_counter()
    acc_passes = None
    for f in files:
        cmd = [ZC, "-O%d" % level, "-S", "-o", out]
        if profile:
            cmd.append("--profile-opts")
        cmd.append(f)
        p = subprocess.run(cmd, capture_output=True, text=True)
        if p.returncode != 0:
            raise RuntimeError("zerocc -O%d failed on %s: %s" %
                               (level, f, p.stderr.strip()[:300]))
        if profile:
            acc_passes = merge_passes(acc_passes, parse_passes(p.stderr))
    return time.perf_counter() - t0, acc_passes


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--levels", type=int, nargs="+", default=[0, 2])
    ap.add_argument("--runs", type=int, default=None,
                    help="override the per-case run count")
    ap.add_argument("--only", default=None, help="comma-separated case names")
    ap.add_argument("--tolerance-wall", type=float, default=TOL_WALL)
    ap.add_argument("--tolerance-pass", type=float, default=TOL_PASS)
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--update-baseline", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(ZC):
        print("error: %s missing (make build/bin/zerocc)" % ZC, file=sys.stderr)
        return 2

    only = None
    if args.only:
        only = set(args.only.split(","))

    baseline = {}
    if os.path.exists(BASELINE):
        baseline = json.load(open(BASELINE)).get("cases", {})

    tmp = tempfile.mkdtemp(prefix="zerocc_perf_")
    results = {}
    setup_errors = []
    try:
        for name in sorted(CASES):
            if only is not None and name not in only:
                continue
            spec = CASES[name]
            runs = args.runs or spec["runs"]
            for lvl in args.levels:
                tag = "O%d" % lvl
                try:
                    wall, passes = measure_case(name, spec, lvl, runs, tmp)
                except Exception as ex:
                    setup_errors.append("%s@%s: %s" % (name, tag, ex))
                    continue
                results["%s@%s" % (name, tag)] = {
                    "wall_s": wall, "passes_ms": passes, "seed_case": name,
                }
                if args.verbose:
                    extra = ""
                    if passes:
                        top = sorted(passes.items(), key=lambda kv: -kv[1])[:3]
                        extra = "  top: " + ", ".join("%s %.1fms" % kv for kv in top)
                    print("[%s] %s@-O%d: wall %.3fs (median of %d)%s" %
                          ("case", name, lvl, wall, runs, extra))
    finally:
        pass

    import shutil
    shutil.rmtree(tmp, ignore_errors=True)

    if setup_errors:
        for e in setup_errors:
            print("SETUP-ERROR %s" % e)
        return 2

    if args.update_baseline:
        os.makedirs(PERF_DIR, exist_ok=True)
        newbase = {}
        for key, r in results.items():
            name, tag = key.rsplit("@", 1)
            e = {"file": CASES[name].get("file"), "runs": args.runs or CASES[name]["runs"]}
            if CASES[name].get("torture"):
                e["torture_sample"] = torture_files()
            e["wall_s"] = round(r["wall_s"], 4)
            if r["passes_ms"]:
                e["passes_ms"] = {k: round(v, 3)
                                  for k, v in sorted(r["passes_ms"].items())}
            newbase[key] = e
        with open(BASELINE, "w") as f:
            json.dump({"generated_by": "tools/verify/perf_budget.py --update-baseline",
                       "note": "ratchet: per-case compile budget (wall_s, host wall +-50%% tolerance; passes_ms, in-process 25%% tolerance). Median of N runs. Re-record after an intentional performance change.",
                       "tolerance_wall": args.tolerance_wall,
                       "tolerance_pass": args.tolerance_pass,
                       "levels": args.levels,
                       "cases": newbase}, f, indent=2)
        print("baseline updated: %d case-levels" % len(newbase))
        return 0

    # gate: compare against the ratcheted budget
    failures = []
    improvements = []
    for key in sorted(results):
        r = results[key]
        b = baseline.get(key)
        if b is None:
            failures.append("%s: not in baseline (new case/level - "
                            "--update-baseline)" % key)
            continue
        # wall
        bw = b.get("wall_s")
        if bw is not None:
            if r["wall_s"] > bw * (1 + args.tolerance_wall):
                failures.append("%s wall %.3fs > budget %.3fs x(1+%.2f)" %
                                (key, r["wall_s"], bw, args.tolerance_wall))
            elif r["wall_s"] < bw * (1 - 0.10):
                improvements.append("%s wall %.3fs (budget %.3fs, %.0f%% under)" %
                                    (key, r["wall_s"], bw,
                                     100 * (1 - r["wall_s"] / bw)))
        # per-pass
        bp = b.get("passes_ms")
        rp = r["passes_ms"]
        if bp is not None or rp is not None:
            bp = bp or {}
            rp = rp or {}
            for k, bv in bp.items():
                if k == "TOTAL (passes only)":
                    continue
                rv = rp.get(k, 0.0)
                # max(40%, 3ms absolute): per-pass times on the large
                # single-function workloads jitter +-20% even in-process
                if rv > bv + max(args.tolerance_pass * bv, PASS_FLOOR):
                    failures.append("%s pass %s %.3fms > budget %.3fms" %
                                    (key, k, rv, bv))
            for k in rp:
                if k not in bp:
                    failures.append("%s new pass %s (%.3fms) not in baseline" %
                                    (key, k, rp[k]))
            bt = bp.get("TOTAL (passes only)")
            rt = rp.get("TOTAL (passes only)")
            if bt and rt is not None and rt > bt * (1 + args.tolerance_pass):
                failures.append("%s TOTAL passes %.3fms > budget %.3fms" %
                                (key, rt, bt))

    for key, r in sorted(results.items()):
        b = baseline.get(key, {})
        bw = b.get("wall_s")
        ratio = (100.0 * r["wall_s"] / bw) if bw else 0
        ptot = (r["passes_ms"] or {}).get("TOTAL (passes only)")
        btot = (b.get("passes_ms") or {}).get("TOTAL (passes only)")
        pratio = (100.0 * ptot / btot) if (ptot is not None and btot) else None
        print("%-28s wall %8.3fs (budget %7.3fs, %5.1f%%)%s" %
              (key, r["wall_s"], bw or 0, ratio,
               (", passes %7.1fms (budget %7.1fms, %5.1f%%)" %
                (ptot, btot, pratio)) if pratio is not None else ""))
    for i in improvements:
        print("improvement: %s" % i)
    for f_ in failures:
        print("FAIL %s" % f_)
    if failures:
        print("GATE FAILED (%d over budget / missing)" % len(failures))
        return 1
    print("gate OK: %d case-levels within budget (%d improvement(s))" %
          (len(results), len(improvements)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
