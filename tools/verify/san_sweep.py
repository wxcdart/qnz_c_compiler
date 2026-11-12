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
import sys
from pathlib import Path

# Make the tools package importable so we can use find_repo_root.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from tools import find_repo_root

import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = find_repo_root()
SAN_DIR = os.path.join(ROOT, "test", "san")
LEVELS = [0, 1, 2, 3]
SIGILL_RC = 132  # 128 + SIGILL(4)
CROSS_ISAS = ["aarch64", "riscv64", "riscv32"]
CROSS_QEMU = {"aarch64": "qemu-aarch64-static",
              "riscv64": "qemu-riscv64-static",
              "riscv32": "qemu-riscv32-static"}
# Expected trap exit status (128+signal) per ISA when a signed overflow fires:
# aarch64 emits brk #0 -> SIGTRAP(5); riscv64/riscv32 emit unimp -> SIGILL(4).
CROSS_TRAP_RC = {"aarch64": 133, "riscv64": 132, "riscv32": 132}
# ISAs whose 64-bit (width-8) check is implemented (riscv32 64-bit is a
# (lo,hi) GPR pair -> documented false-negative, so the wide test is skipped).
CROSS_WIDE_ISAS = ["aarch64", "riscv64"]


# Text-AOT (-S) path link recipes (Sprint 165): zerocc -S, cross-assemble, static
# link. aarch64/riscv64 use the system cross gcc; riscv32 has no Debian rv32 libc,
# so it assembles with the rv64 cross gcc (-march=rv32gc) and links explicitly
# against the static-musl rv32 prefix ($ZC_RV32_MUSL or the in-tree default).
CROSS_ASM_CC = {
    "aarch64": ["aarch64-linux-gnu-gcc", "-mabi=lp64", "-march=armv8-a"],
    "riscv64": ["riscv64-linux-gnu-gcc", "-mabi=lp64d", "-march=rv64gc"],
    "riscv32": ["riscv64-linux-gnu-gcc", "-mabi=ilp32", "-march=rv32gc"],
}
PATHS = ["raw", "asm"]


def rv32_prefix():
    # Static-musl rv32 prefix. The Debian riscv64 cross gcc ships only an rv64
    # libgcc (ABI-incompatible with elf32), so linking an rv32 static executable
    # needs the committed static-musl crt/libc/libgcc from
    # third_party/vendor/musl-cross/riscv32 (built by tools/setup_musl_cross.sh,
    # also mirrored at /home/ubuntu/dev/musl-cross/riscv32 in this environment).
    cand = [os.environ.get("ZC_RV32_MUSL", ""),
            os.path.join(ROOT, "third_party", "vendor", "musl-cross", "riscv32"),
            "/home/ubuntu/dev/musl-cross/riscv32"]
    needed = ["Scrt1.o", "crti.o", "crtn.o", "libc.a", "libgcc.a"]
    for c in cand:
        if not c:
            continue
        lib = os.path.join(c, "lib")
        if all(os.path.isfile(os.path.join(lib, f)) for f in needed):
            return c
    return None


def build_cross(zerocc, isa, level, src, out, path):
    """Compile src for `isa` with -fsanitize=undefined via the raw AOT path or
    the text (-S + cross-assemble + static link) path. Returns (rc, err)."""
    flags = ["--target", isa, f"-O{level}", "-fsanitize=undefined"]
    if path == "raw":
        rc, _, err = run([zerocc] + flags + [src, "-o", out])
        return rc, err
    asm = out + ".s"
    rc, _, err = run([zerocc] + flags + ["-S", src, "-o", asm])
    if rc != 0:
        return rc, err
    cc = CROSS_ASM_CC[isa]
    if shutil.which(cc[0]) is None:
        return 1, f"{cc[0]} not found"
    if isa != "riscv32":
        rc, _, err = run(cc + ["-static", asm, "-o", out])
        return rc, err
    pfx = rv32_prefix()
    if pfx is None:
        return 1, "RV32 musl prefix not found (set ZC_RV32_MUSL)"
    obj = out + ".o"
    rc, _, err = run(cc + ["-c", asm, "-o", obj])
    if rc != 0:
        return rc, err
    lib = os.path.join(pfx, "lib")
    rc, _, err = run(cc + ["-static", "-no-pie", "-Wl,-m,elf32lriscv", "-nostdlib",
                           os.path.join(lib, "Scrt1.o"), os.path.join(lib, "crti.o"), obj,
                           os.path.join(lib, "libc.a"), os.path.join(lib, "libgcc.a"),
                           os.path.join(lib, "crtn.o"), "-o", out])
    return rc, err


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
            for path in PATHS:
                out = os.path.join(tmpdir, f"cross_{isa}_{path}")
                rc, err = build_cross(args.zerocc, isa, 1, cross_src, out, path)
                if rc != 0:
                    fails += 1
                    results.append({"test": "cross", "isa": isa, "path": path,
                                    "status": "FAIL",
                                    "detail": f"compile rc={rc}: {err.strip()[:200]}"})
                    continue
                rc, outp, err = run([qemu, out])
                ok = rc == 0
                if not ok:
                    fails += 1
                cross_done += 1
                results.append({"test": "cross", "isa": isa, "path": path, "rc": rc,
                                "expect": 0, "status": "ok" if ok else "FAIL",
                                "detail": err.strip()[:200]})

    # Cross-ISA trap-firing (Sprint 163): a real signed overflow must TRAP under
    # qemu on every ISA with a check. 32-bit on all three; 64-bit on aarch64 /
    # riscv64 only (riscv32 width-8 is a documented false-negative follow-up).
    cross_trap = [("san_trap_cross_int.c", CROSS_ISAS),
                  ("san_trap_cross_incdec.c", CROSS_ISAS),
                  ("san_trap_cross_cassign.c", CROSS_ISAS),
                  ("san_trap_cross_wide.c", CROSS_WIDE_ISAS)]
    if not args.no_cross:
        for tname, isas in cross_trap:
            tsrc = os.path.join(SAN_DIR, tname)
            if not os.path.isfile(tsrc):
                continue
            for isa in isas:
                qemu = CROSS_QEMU[isa]
                if shutil.which(qemu) is None:
                    results.append({"test": tname, "isa": isa, "status": "skip"})
                    continue
                for level in LEVELS:
                    for path in PATHS:
                        out = os.path.join(tmpdir, f"xtrap_{isa}_{level}_{path}")
                        rc, err = build_cross(args.zerocc, isa, level, tsrc, out, path)
                    if rc != 0:
                        fails += 1
                        results.append({"test": tname, "isa": isa, "level": level,
                                        "path": path, "status": "FAIL",
                                        "detail": f"compile rc={rc}: {err.strip()[:200]}"})
                        continue
                    rc, _, err = run([qemu, out])
                    ok = rc in (CROSS_TRAP_RC[isa], 128 - CROSS_TRAP_RC[isa])  # shell 128+sig or Popen -sig
                    if not ok:
                        fails += 1
                    results.append({"test": tname, "isa": isa, "level": level,
                                    "path": path, "rc": rc,
                                    "expect": CROSS_TRAP_RC[isa],
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
