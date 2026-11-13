#!/usr/bin/env python3
"""
-E Preprocessor Differential Gate (Sprint 176, dev-plan Verification &
Scale item 4)

Differential-tests zerocc's preprocessor against a reference gcc by
comparing `zerocc -E` vs `gcc -E` output (normalized) over the
macro-heavy corpus test/preproc/*.c.

Why: preprocessor bugs (argument expansion before #/##, pp-number
atomicity, stringification/pasting edge cases, __LINE__ tracking,
pragma handling) only surface when an execute test happens to exercise
the expansion. The -E diff catches them directly, in isolation.

Normalization (applied to BOTH sides before comparison):
  - drop gcc's `# <n> "<file>" ...` line markers (zerocc emits none)
  - drop blank lines (gcc replaces a consumed directive line with a
    newline, zerocc drops it - line-structure cosmetics, not semantics)
  - collapse runs of spaces/tabs to one, outside string/char literals
    (gcc re-serializes macro-expansion tokens with single spaces, zerocc
    preserves character-stream positions - whitespace between tokens is
    never semantic in -E output; literal content is left untouched)
  - strip trailing whitespace
  - replace the main file's path (relative or absolute form) inside
    string literals with <FILE> (gcc emits the path as given, zerocc
    may emit its canonical form)

Gate semantics (baseline ratchet, test/preproc/baseline.json):
  * a case is GREEN when the normalized outputs are byte-identical
  * a case whose outputs differ is recorded as a known-diff with a
    signature (sha1 of the unified diff) - the gate FAILS if a green
    case starts differing, or if a recorded diff's signature CHANGES
    (the divergence got worse or different)
  * a recorded diff that now matches is an improvement (non-failing,
    re-record to ratchet it)
  * --update-baseline re-records the current state

Usage:
  python3 tools/verify/preproc_diff.py
  python3 tools/verify/preproc_diff.py --only stringify
  python3 tools/verify/preproc_diff.py --verbose
  python3 tools/verify/preproc_diff.py --update-baseline
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from tools import find_repo_root

ROOT = find_repo_root()
TEST_DIR = os.path.join(str(ROOT), "test", "preproc")
BASELINE = os.path.join(TEST_DIR, "baseline.json")
ZEROCC = os.environ.get("ZEROCC", os.path.join(str(ROOT), "build", "bin", "zerocc"))
TIMEOUT = 120

LINE_MARKER = re.compile(r"^#\s*\d")


def collapse_ws(line):
    """Collapse runs of spaces/tabs to one, OUTSIDE string/char literals
    (literal content may be whitespace-significant)."""
    out = []
    i = 0
    n = len(line)
    in_str = None  # the quote char when inside a literal
    while i < n:
        ch = line[i]
        if in_str:
            out.append(ch)
            if ch == "\\" and i + 1 < n:
                out.append(line[i + 1])
                i += 2
                continue
            if ch == in_str:
                in_str = None
            i += 1
            continue
        if ch == '"' or ch == "'":
            in_str = ch
            out.append(ch)
            i += 1
            continue
        if ch == " " or ch == "\t":
            while i < n and line[i] in " \t":
                i += 1
            if out:
                out.append(" ")
            continue
        out.append(ch)
        i += 1
    return "".join(out).rstrip()


def pick_gcc():
    if os.environ.get("PREPROC_GCC"):
        return os.environ["PREPROC_GCC"]
    for cand in ("gcc-15", "gcc-14", "gcc"):
        if shutil.which(cand):
            return cand
    return "gcc"


# The corpus is written against zerocc's default dialect (gnu11); the
# reference gets the same dialect explicitly (gcc-15's own default is
# gnu17, whose __STDC_VERSION__ would diverge).
STD_FLAG = "-std=gnu11"


def run(cmd, cwd):
    try:
        r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                           timeout=TIMEOUT)
        return r.returncode, r.stdout, r.stderr
    except subprocess.TimeoutExpired:
        return None, "", "timeout"


def normalize(text, main_rel, main_abs):
    out = []
    for line in text.splitlines():
        if LINE_MARKER.match(line):
            continue
        line = collapse_ws(line)
        if not line:
            continue
        line = line.replace('"{}"'.format(main_abs), '"<FILE>"')
        line = line.replace('"{}"'.format(main_rel), '"<FILE>"')
        out.append(line)
    return out


def unified_diff(a, b):
    import difflib
    return "\n".join(difflib.unified_diff(a, b, "zerocc", "gcc",
                                          lineterm=""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", help="substring filter on test name")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--update-baseline", action="store_true")
    args = ap.parse_args()

    gcc = pick_gcc()
    if not os.path.exists(ZEROCC):
        print("error: build build/bin/zerocc first", file=sys.stderr)
        return 2
    if not shutil.which(gcc):
        print("error: reference gcc not found (set PREPROC_GCC)", file=sys.stderr)
        return 2

    tests = sorted(Path(TEST_DIR).glob("*.c")) if os.path.isdir(TEST_DIR) else []
    if not tests:
        print("error: no tests under test/preproc/", file=sys.stderr)
        return 2
    if args.only:
        tests = [t for t in tests if args.only in t.name]

    baseline = {}
    if os.path.exists(BASELINE) and not args.update_baseline:
        with open(BASELINE) as f:
            baseline = json.load(f).get("cases", {})

    results = {}
    failed = []
    improved = []

    for t in tests:
        name = t.name[:-2]
        rel = os.path.relpath(str(t), str(ROOT))
        abs_path = str(t.resolve())
        rc_z, out_z, err_z = run([ZEROCC, "-E", STD_FLAG, rel], cwd=str(ROOT))
        rc_g, out_g, err_g = run([gcc, "-E", STD_FLAG, rel], cwd=str(ROOT))
        if rc_z is None or rc_g is None:
            failed.append((name, "timeout"))
            continue
        if rc_g != 0:
            # The reference must accept the corpus case - a gcc diagnostic
            # means the case itself is ill-formed (diagnostic-diff territory
            # belongs to tools/verify/diag_battery.py, not this gate).
            failed.append((name, "gcc -E failed: " +
                           (err_g.strip().splitlines() or ["?"])[0]))
            continue
        if rc_z != 0:
            failed.append((name, "zerocc -E failed: " +
                           (err_z.strip().splitlines() or ["?"])[0]))
            continue
        nz = normalize(out_z, rel, abs_path)
        ng = normalize(out_g, rel, abs_path)
        if nz == ng:
            results[name] = {"status": "pass"}
            if baseline.get(name, {}).get("status") == "diff":
                improved.append(name)
        else:
            sig = hashlib.sha1(unified_diff(nz, ng).encode()).hexdigest()[:16]
            results[name] = {"status": "diff", "signature": sig}
            rec = baseline.get(name)
            if not (rec and rec.get("status") == "diff" and
                    rec.get("signature") == sig):
                failed.append((name, "output differs from gcc" +
                               (" (changed diff)" if rec else "")))
                if args.verbose:
                    print(unified_diff(nz, ng))

    if args.update_baseline:
        payload = {
            "generated_by": "tools/verify/preproc_diff.py --update-baseline",
            "note": ("ratchet baseline: normalized `zerocc -E` vs `gcc -E` "
                     "output (line markers / blank lines / trailing ws "
                     "stripped, __FILE__ path canonicalized). A 'diff' "
                     "entry records a known divergence by signature; the "
                     "gate fails on a new or changed divergence. Re-record "
                     "after an intentional improvement."),
            "reference": gcc,
            "cases": results,
        }
        with open(BASELINE, "w") as f:
            json.dump(payload, f, indent=2)
            f.write("\n")
        print("baseline updated: {} cases".format(len(results)))
        return 0

    n_pass = sum(1 for r in results.values() if r["status"] == "pass")
    n_diff = len(results) - n_pass
    for name in improved:
        print("[IMPROVED] {}".format(name) +
              " (re-run --update-baseline to ratchet)")
    if failed:
        for name, why in failed:
            print("[FAIL] {} - {}".format(name, why))
        print("\n{} cases: {} match, {} known-diff, {} FAILED "
              "(reference: {})".format(len(results), n_pass, n_diff,
                                        len(failed), gcc))
        return 1
    print("{} cases: all normalized -E output matches {} ({} known-diff "
          "ratcheted)".format(len(results), gcc, n_diff))
    return 0


if __name__ == "__main__":
    sys.exit(main())
