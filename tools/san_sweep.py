#!/usr/bin/env python3
"""
san_sweep.py - zero_san v1 (-fsanitize=undefined) regression sweep

Compiles every test/san/*.c with zerocc at -O0..-O3 in BOTH JIT (--run) and
AOT (-o) modes, with -fsanitize=undefined, and asserts the expected outcome
from the file-name contract:

  san_ok_*.c   -> exit 0 (a valid program must never false-trap). Also run
                  WITHOUT the flag to prove the instrumentation is
                  behavior-preserving on defined code.
  san_trap_*.c -> exit 132 (SIGILL: a real signed overflow must trap at every
                  -O level - guards the pass interaction: LICM/DCE/CSE must
                  not hoist, drop, or orphan the check).

Cross-ISA smoke: san_ok_no_overflow.c is compiled with the flag for
aarch64/riscv64/riscv32 (raw-AOT default object path) and run under
qemu-*-static; it must exit 0 - guards the invariant that no x86 check bytes
leak into other ISAs' binaries and that the check is cleanly skipped there.
Skipped (not failed) when a qemu binary is missing.

Why this gate exists: `make test` builds test binaries with the host gcc and
`make test-opt` runs without the flag, so neither exercises zero_san at all.

Exit status: 0 = all ok, 1 = at least one FAIL.

Usage:
    python3 tools/san_sweep.py [--zerocc PATH] [--no-cross] [--json]
"""

import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SAN_DIR = os.path.join(ROOT, "test", "san")
LEVELS = [0, 1, 2, 3]
SIGILL_RC = 132  # 128 + SIGILL(4)
CROSS_ISAS = ["aarch64", "riscv64", "riscv32"]
CROSS_QEMU = {"aarch64": "qemu-aarch64-static",
              "riscv64": "qemu-riscv64-static",
              "riscv32": "qemu-riscv32-static"}


def run(cmd, timeout=30):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return "timeout", "", ""


def expected_rc(name):
    if name.startswith("san_ok_"):
        return 0
    if name.startswith("san_trap_"):
        return SIGILL_RC
    return None


def is_sigill(rc, expected):
    if expected != SIGILL_RC:
        return rc == expected
    # subprocess reports a signal death as -signum; the shell form is 128+sig
    return rc == SIGILL_RC or rc == -4


def run_jit(zerocc, src, level, flag, timeout=30):
    cmd = [zerocc, "--run", f"-O{level}"]
    if flag:
        cmd.append("-fsanitize=undefined")
    cmd.append(src)
    return run(cmd, timeout)


def run_aot(zerocc, src, level, flag, tmpdir, timeout=60):
    out = os.path.join(tmpdir, "aot_bin")
    cmd = [zerocc, f"-O{level}"]
    if flag:
        cmd.append("-fsanitize=undefined")
    cmd += [src, "-o", out]
    rc, _, err = run(cmd, timeout)
    if rc != 0:
        return rc, "", err
    return run([out], timeout)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--zerocc", default=os.path.join(ROOT, "build", "bin", "zerocc"))
    ap.add_argument("--no-cross", action="store_true", help="skip the cross-ISA qemu smoke")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    if not os.path.isfile(args.zerocc):
        print(f"error: zerocc not found at {args.zerocc}", file=sys.stderr)
        return 2

    sources = sorted(glob.glob(os.path.join(SAN_DIR, "*.c")))
    if not sources:
        print(f"error: no tests in {SAN_DIR}", file=sys.stderr)
        return 2

    results = []
    fails = 0
    tmpdir = tempfile.mkdtemp(prefix="san_sweep_")

    for src in sources:
        name = os.path.basename(src)
        exp = expected_rc(name)
        if exp is None:
            print(f"  {name}: no san_ok_/san_trap_ prefix - SKIPPED")
            results.append({"test": name, "status": "skip", "detail": "bad name"})
            continue
        for level in LEVELS:
            for mode, runner in (("jit", lambda l: run_jit(args.zerocc, src, l, True)),
                                 ("aot", lambda l: run_aot(args.zerocc, src, l, True, tmpdir))):
                rc, out, err = runner(level)
                ok = is_sigill(rc, exp)
                if not ok:
                    fails += 1
                results.append({"test": name, "level": level, "mode": mode,
                                "flag": True, "rc": rc, "expect": exp,
                                "status": "ok" if ok else "FAIL",
                                "detail": err.strip()[:200]})
        # ok tests: the flag must be behavior-preserving on defined code
        if exp == 0:
            for mode, runner in (("jit", lambda l: run_jit(args.zerocc, src, 0, False)),
                                 ("aot", lambda l: run_aot(args.zerocc, src, 0, False, tmpdir))):
                rc, out, err = runner(0)
                ok = rc == 0
                if not ok:
                    fails += 1
                results.append({"test": name, "level": 0, "mode": mode,
                                "flag": False, "rc": rc, "expect": 0,
                                "status": "ok" if ok else "FAIL",
                                "detail": err.strip()[:200]})

    # Cross-ISA smoke: valid code with the flag must run clean (checks skipped)
    cross_done = 0
    cross_src = os.path.join(SAN_DIR, "san_ok_cross_smoke.c")
    if not args.no_cross and os.path.isfile(cross_src):
        for isa in CROSS_ISAS:
            qemu = CROSS_QEMU[isa]
            if shutil.which(qemu) is None:
                print(f"  cross {isa}: qemu ({qemu}) not found - SKIPPED")
                results.append({"test": "cross", "isa": isa, "status": "skip"})
                continue
            out = os.path.join(tmpdir, f"cross_{isa}")
            rc, _, err = run([args.zerocc, "--target", isa, "-O1",
                              "-fsanitize=undefined", cross_src, "-o", out])
            if rc != 0:
                fails += 1
                results.append({"test": "cross", "isa": isa, "status": "FAIL",
                                "detail": f"compile rc={rc}: {err.strip()[:200]}"})
                continue
            rc, outp, err = run([qemu, out])
            ok = rc == 0
            if not ok:
                fails += 1
            cross_done += 1
            results.append({"test": "cross", "isa": isa, "rc": rc, "expect": 0,
                            "status": "ok" if ok else "FAIL",
                            "detail": err.strip()[:200]})

    shutil.rmtree(tmpdir, ignore_errors=True)

    if args.json:
        print(json.dumps({"results": results, "fails": fails}, indent=1))
    else:
        for r in results:
            if r["status"] != "ok":
                print(f"  {r}")
        n_ok = sum(1 for r in results if r["status"] == "ok")
        print(f"san sweep: {n_ok} ok, {fails} FAIL "
              f"({len([r for r in results if 'isa' in r and r.get('status') == 'ok'])} cross), "
              f"{len([r for r in results if r.get('flag') is True])} flagged runs")

    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
