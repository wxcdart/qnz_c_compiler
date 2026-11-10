#!/usr/bin/env python3
"""
loop_unroll_verifier.py - Loop full-unrolling gate (Sprint 150, plan item 3)

Differential-tests the -O3 loop-unrolling pass (src/zero_cc/opt/
opt_loop_unroll.c) on the test/jit/unroll_*.c corpus:

  1. Every file is compiled and run with host gcc -O2 (reference),
     then with zerocc at -O0 (baseline) and -O3 in BOTH JIT (--run)
     and AOT (-o + exec) modes.  Exit code + stdout must match the
     reference everywhere: unrolling is an optimization, never a
     semantic change (break/continue/early-return/pointer-step
     semantics are the class this gate pins).
  2. -O1/-O2 JIT runs must agree too (the pass is -O3-gated, so these
     pin that nothing else at those levels is disturbed).
  3. The pass's own accounting is checked through --profile-opts
     ("Loops fully unrolled: N"): files marked `// expect-unroll: yes`
     must report N >= 1 at -O3 (the transform really fired), files
     marked `// expect-unroll: no` must report N == 0 (every gate
     held - variable bound, trip > 8, calls, do-while, VLA, user
     labels in the span, ...).  Unmarked files are correctness-only.

Why this gate exists: `make test-opt` sweeps the whole corpus at every
level, but only via behavior - it cannot distinguish "correctly
unrolled" from "correctly skipped", so a pass that silently stops
firing (or fires on a disqualified loop) would pass it.

Reference compiler: UNROLL_GCC env override, else gcc-15, else gcc-14,
else gcc.

Usage:
    python3 tools/loop_unroll_verifier.py [--zerocc PATH] [--verbose]
"""

import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORPUS_DIR = os.path.join(ROOT, "test", "jit")

ZEROCC = os.environ.get("ZEROCC", os.path.join(ROOT, "build", "bin", "zerocc"))
GCC = os.environ.get("UNROLL_GCC") or next(
    (c for c in ("gcc-15", "gcc-14", "gcc") if shutil.which(c)), "gcc")

UNROLL_RE = re.compile(r"Loops fully unrolled:\s*(\d+)")
MARK_RE = re.compile(r"^\s*//\s*expect-unroll:\s*(yes|no)", re.MULTILINE)


def run(cmd, timeout=30):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return "timeout", "", ""


def expect_unroll(src_text):
    m = MARK_RE.search(src_text)
    return None if not m else (m.group(1) == "yes")


def ref_run(path):
    tmp = tempfile.mktemp(prefix="lu_ref_")
    rc, out, err = run([GCC, "-O2", "-w", path, "-o", tmp], timeout=30)
    if rc != 0:
        return rc, out, err
    rc2, out2, err2 = run([tmp], timeout=30)
    os.unlink(tmp)
    return rc2, out2, err2


def zerocc_jit(path, level, extra=()):
    return run([ZEROCC, "-O%d" % level, "--run", path] + list(extra), timeout=30)


def zerocc_jit_count(path, level):
    rc, _out, err = run([ZEROCC, "-O%d" % level, "--run", "--profile-opts", path],
                        timeout=30)
    m = UNROLL_RE.search(err)
    return rc, (int(m.group(1)) if m else None)


def zerocc_aot(path, level):
    tmp = tempfile.mktemp(prefix="lu_aot_")
    rc, out, err = run([ZEROCC, "-O%d" % level, path, "-o", tmp], timeout=30)
    if rc != 0:
        return rc, out, err
    rc2, out2, err2 = run([tmp], timeout=30)
    os.unlink(tmp)
    return rc2, out2, err2


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--zerocc", default=ZEROCC)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    if args.zerocc != ZEROCC:
        globals()["ZEROCC"] = args.zerocc

    files = sorted(glob.glob(os.path.join(CORPUS_DIR, "unroll_*.c")))
    if not files:
        print("FAIL: no corpus files under %s" % CORPUS_DIR)
        return 1
    if not shutil.which(GCC):
        print("FAIL: reference compiler %r not found" % GCC)
        return 1

    ok = failed = 0
    results = {}
    for path in files:
        name = os.path.basename(path)
        with open(path) as f:
            src = f.read()
        expect = expect_unroll(src)

        checks = []  # (label, rc, out)
        checks.append(("gcc", *ref_run(path)[:2]))

        base_rc, base_out, _ = zerocc_jit(path, 0)
        checks.append(("O0-jit", base_rc, base_out))
        for lvl in (1, 2, 3):
            rc, out, err = zerocc_jit(path, lvl)
            checks.append(("O%d-jit" % lvl, rc, out))
        rc, out, err = zerocc_aot(path, 3)
        checks.append(("O3-aot", rc, out))

        # reference must run cleanly (it defines the expected output);
        # for GNU-extension files gcc cannot compile (e.g. nonlocal
        # gotos) fall back to the zerocc -O0 baseline, per the gate's
        # "differential vs -O0/-O2 baselines" design.
        ref_rc, ref_out = checks[0][1], checks[0][2]
        errs = []
        notes = []
        if ref_rc != 0:
            if base_rc == 0:
                ref_rc, ref_out = base_rc, base_out
                notes.append("reference gcc failed; using -O0 baseline")
            else:
                errs.append("reference gcc failed (rc=%r)" % ref_rc)
        for label, rc, out in checks[1:]:
            if rc != ref_rc or out != ref_out:
                errs.append("%s mismatch: rc=%r out=%r (want rc=%r out=%r)"
                            % (label, rc, out.strip(), ref_rc, ref_out.strip()))

        # unroll accounting at -O3
        count_rc, count = zerocc_jit_count(path, 3)
        if count_rc != 0:
            errs.append("profile run failed (rc=%r)" % count_rc)
        elif count is None:
            errs.append("profile output missing unroll counter")
        elif expect is True and count < 1:
            errs.append("expected >=1 unrolled loop, got %d" % count)
        elif expect is False and count != 0:
            errs.append("expected 0 unrolled loops, got %d" % count)

        status = "ok" if not errs else "FAIL"
        if errs:
            failed += 1
        else:
            ok += 1
        results[name] = {"status": status, "expect_unroll": expect,
                         "unrolled": count, "errors": errs, "notes": notes}
        line = "%-28s %-4s unrolled=%s" % (name, status,
                                           "-" if count is None else count)
        for e in notes:
            line += "  (%s)" % e
        if args.verbose or errs:
            for e in errs:
                line += "\n    " + e
        print(line)

    print("\n%d ok, %d failed (%d files, reference: %s)"
          % (ok, failed, len(files), GCC))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
