#!/usr/bin/env python3
"""
Inline-Assembly Differential Gate (Sprint 135 Phase 1 / Sprint 136 Phase 2)

Differential-tests x86-64 inline asm across three execution paths:
- AOT text path: `-S` -> host assembler -> link -> run
- JIT path: `--run` (Phase 2 integrated inline assembler)
- Raw binary AOT path: `ZC_RAW_AOT=1` (Phase 2 integrated assembler + own ELF writer)

Every test/asm/*.c is compiled with ZeroCC at -O0..-O3 across all selected
paths, and compared (exit code + stdout) against a host-`gcc -O2` reference.

Usage:
  python3 tools/asm_diff.py
  python3 tools/asm_diff.py --paths jit raw
  python3 tools/asm_diff.py --levels 0 2
  python3 tools/asm_diff.py --only asm_basic
"""
import sys
from pathlib import Path

# Make the tools package importable so we can use find_repo_root.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from tools import find_repo_root

import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile

ROOT = find_repo_root()
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


def run(cmd, env=None, timeout=TIMEOUT):
    try:
        return subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--levels", nargs="*", type=int, default=[0, 1, 2, 3])
    ap.add_argument("--paths", nargs="*", default=["text", "jit", "raw"],
                    choices=["text", "jit", "raw"],
                    help="execution paths to test (text=-S+gas, jit=--run, raw=ZC_RAW_AOT=1)")
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
            if "text" in args.paths:
                s = os.path.join(tmp, f"{name}_o{lvl}.s")
                o = os.path.join(tmp, f"{name}_o{lvl}.o")
                b = os.path.join(tmp, f"{name}_o{lvl}")
                rc = run([ZEROCC, f"-O{lvl}", "-S", path, "-o", s])
                if rc is None or rc.returncode != 0:
                    line = (rc.stderr or "").strip().splitlines()
                    msg = line[0] if line else "timeout"
                    failed.append((name, f"text-O{lvl}", "compile", msg))
                    if args.verbose:
                        print(f"[FAIL] {name} text-O{lvl}: compile: {msg}")
                    continue
                rc = run([cc, "-c", s, "-o", o])
                if rc is None or rc.returncode != 0:
                    line = (rc.stderr or "").strip().splitlines()
                    msg = line[0] if line else "timeout"
                    failed.append((name, f"text-O{lvl}", "assemble", msg))
                    if args.verbose:
                        print(f"[FAIL] {name} text-O{lvl}: assemble: {msg}")
                    continue
                rc = run([cc, o, "-o", b])
                if rc is None or rc.returncode != 0:
                    failed.append((name, f"text-O{lvl}", "link", (rc.stderr or "").strip().splitlines()[0]))
                    continue
                rr = run([b])
                if rr is None:
                    failed.append((name, f"text-O{lvl}", "run", "timeout"))
                    continue
                got = (rr.returncode, rr.stdout)
                if got != ref:
                    failed.append((name, f"text-O{lvl}", "diff",
                                   f"got rc={got[0]} out={got[1]!r} want rc={ref[0]} out={ref[1]!r}"))
                    if args.verbose:
                        print(f"[FAIL] {name} text-O{lvl}: {failed[-1][3]}")
                    continue
                ok += 1
                if args.verbose:
                    print(f"[ok ] {name} text-O{lvl}")

            if "jit" in args.paths:
                rr = run([ZEROCC, f"-O{lvl}", "--run", path])
                if rr is None:
                    failed.append((name, f"jit-O{lvl}", "run", "timeout"))
                    continue
                got = (rr.returncode, rr.stdout)
                if got != ref:
                    msg = (rr.stderr or "").strip().splitlines()
                    err = msg[0] if msg else f"got rc={got[0]} out={got[1]!r} want rc={ref[0]} out={ref[1]!r}"
                    failed.append((name, f"jit-O{lvl}", "diff", err))
                    if args.verbose:
                        print(f"[FAIL] {name} jit-O{lvl}: {err}")
                    continue
                ok += 1
                if args.verbose:
                    print(f"[ok ] {name} jit-O{lvl}")

            if "raw" in args.paths:
                b_raw = os.path.join(tmp, f"{name}_raw_o{lvl}")
                env_raw = dict(os.environ, ZC_RAW_AOT="1")
                rc = run([ZEROCC, f"-O{lvl}", path, "-o", b_raw], env=env_raw)
                if rc is None or rc.returncode != 0:
                    line = (rc.stderr or "").strip().splitlines()
                    msg = line[0] if line else "timeout"
                    failed.append((name, f"raw-O{lvl}", "compile", msg))
                    if args.verbose:
                        print(f"[FAIL] {name} raw-O{lvl}: compile: {msg}")
                    continue
                rr = run([b_raw])
                if rr is None:
                    failed.append((name, f"raw-O{lvl}", "run", "timeout"))
                    continue
                got = (rr.returncode, rr.stdout)
                if got != ref:
                    failed.append((name, f"raw-O{lvl}", "diff",
                                   f"got rc={got[0]} out={got[1]!r} want rc={ref[0]} out={ref[1]!r}"))
                    if args.verbose:
                        print(f"[FAIL] {name} raw-O{lvl}: {failed[-1][3]}")
                    continue
                ok += 1
                if args.verbose:
                    print(f"[ok ] {name} raw-O{lvl}")

    total = ok + len(failed)
    print("=" * 60)
    if failed:
        for name, lvl, kind, msg in failed:
            print(f"  [FAIL] {name} -O{lvl} ({kind}): {msg}")
    print(f"Inline-asm gate: {ok}/{total} runs passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
