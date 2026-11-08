#!/usr/bin/env python3
"""
Inline-Assembly Phase 1 Differential Gate (Sprint 135)

Differential-tests the x86-64 AOT *text* path for GNU inline asm
(`-S` -> host assembler -> link -> run). Every test/asm/*.c is compiled
with ZeroCC at -O0..-O3, assembled and linked with the host toolchain,
run, and compared (exit code + stdout) against a host-`gcc -O2` reference
run of the same source.

The JIT / raw-emitter paths intentionally REJECT non-empty templates in
Phase 1 (the integrated assembler is Phase 2); this gate covers the text
path only. The JIT-safe barrier forms are swept by `make test-opt` via
test/jit/inline_asm_barrier.c.

Reference assembler/linker: the host `gcc` (or $ASM_CC). ZeroCC emits
portable AT&T assembly for the host GNU assembler, so any gcc that
targets the build machine works.

Usage:
  python3 tools/asm_diff.py
  python3 tools/asm_diff.py --levels 0 2
  python3 tools/asm_diff.py --only asm_basic
"""

import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASM_DIR = os.path.join(ROOT, "test", "asm")
ZEROCC = os.path.join(ROOT, "build", "bin", "zerocc")
TIMEOUT = 120


def pick_cc():
    if os.environ.get("ASM_CC"):
        return os.environ["ASM_CC"]
    for cand in ("gcc-15", "gcc-14", "gcc"):
        p = shutil_which(cand)
        if p:
            return cand
    return "gcc"


def shutil_which(name):
    import shutil
    return shutil.which(name)


def run(cmd, timeout=TIMEOUT):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--levels", nargs="*", type=int, default=[0, 1, 2, 3])
    ap.add_argument("--only", help="substring filter on test name")
    ap.add_argument("--cc", default=None, help="host assembler/linker driver")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(ZEROCC):
        print("error: build build/bin/zerocc first", file=sys.stderr)
        return 2
    cc = args.cc or pick_cc()

    tests = sorted(glob.glob(os.path.join(ASM_DIR, "*.c")))
    if not tests:
        print("error: no tests under test/asm/", file=sys.stderr)
        return 2
    if args.only:
        tests = [t for t in tests if args.only in os.path.basename(t)]

    ok = 0
    failed = []
    tmp = tempfile.mkdtemp(prefix="zc_asm_")

    for path in tests:
        name = os.path.basename(path)[:-2]
        with open(path) as f:
            src = f.read()

        # Reference: host gcc -O2.
        ref_c = os.path.join(tmp, f"ref_{name}.c")
        with open(ref_c, "w") as f:
            f.write(src)
        ref_bin = os.path.join(tmp, f"ref_{name}")
        rc = run([cc, "-O2", "-o", ref_bin, ref_c])
        if rc is None or rc.returncode != 0:
            print(f"[SKIP] {name}: reference {cc} build failed")
            continue
        rr = run([ref_bin])
        ref = (rr.returncode, rr.stdout)

        for lvl in args.levels:
            s = os.path.join(tmp, f"{name}_o{lvl}.s")
            o = os.path.join(tmp, f"{name}_o{lvl}.o")
            b = os.path.join(tmp, f"{name}_o{lvl}")
            rc = run([ZEROCC, f"-O{lvl}", "-S", path, "-o", s])
            if rc is None or rc.returncode != 0:
                line = (rc.stderr or "").strip().splitlines()
                msg = line[0] if line else "timeout"
                failed.append((name, lvl, "compile", msg))
                if args.verbose:
                    print(f"[FAIL] {name} -O{lvl}: compile: {msg}")
                continue
            rc = run([cc, "-c", s, "-o", o])
            if rc is None or rc.returncode != 0:
                line = (rc.stderr or "").strip().splitlines()
                msg = line[0] if line else "timeout"
                failed.append((name, lvl, "assemble", msg))
                if args.verbose:
                    print(f"[FAIL] {name} -O{lvl}: assemble: {msg}")
                continue
            rc = run([cc, o, "-o", b])
            if rc is None or rc.returncode != 0:
                failed.append((name, lvl, "link", (rc.stderr or "").strip().splitlines()[0]))
                continue
            rr = run([b])
            if rr is None:
                failed.append((name, lvl, "run", "timeout"))
                continue
            got = (rr.returncode, rr.stdout)
            if got != ref:
                failed.append((name, lvl, "diff",
                               f"got rc={got[0]} out={got[1]!r} want rc={ref[0]} out={ref[1]!r}"))
                if args.verbose:
                    print(f"[FAIL] {name} -O{lvl}: {failed[-1][3]}")
                continue
            ok += 1
            if args.verbose:
                print(f"[ok ] {name} -O{lvl}")

    total = ok + len(failed)
    print("=" * 60)
    if failed:
        for name, lvl, kind, msg in failed:
            print(f"  [FAIL] {name} -O{lvl} ({kind}): {msg}")
    print(f"Inline-asm gate: {ok}/{total} runs passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
