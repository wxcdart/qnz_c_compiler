#!/usr/bin/env python3
"""ISA QEMU Differential Harness.

Exercises zerocc's RAW binary emitters for every target ISA by building a
static executable (ZC_RAW_AOT=1 forces the raw binary path for cross-targets)
and diffing its runtime behaviour -- stdout + exit code -- against a reference
built by the system cross-gcc, running BOTH under the matching
`qemu-*-static`. This is the differential test that was missing for the
aarch64/riscv raw emitters (the x86-64 raw path is the default AOT path).

Dependencies (required):
  - QEMU static binaries:  qemu-x86_64-static qemu-aarch64-static
                            qemu-riscv64-static qemu-riscv32-static
  - A uniform STATIC-MUSL reference per ISA, provided by the
    `musl-gcc-<arch>` wrappers installed by `tools/setup_musl_cross.sh`
    (builds static musl + a wrapper using the system cross-gcc for codegen).
    This gives one consistent libc baseline across all ISAs for the differential.

Usage:
    python3 tools/isa_qemu_diff.py                    # all ISAs
    python3 tools/isa_qemu_diff.py --isa aarch64      # one ISA
    python3 tools/isa_qemu_diff.py --only 07_float    # subset by name
    python3 tools/isa_qemu_diff.py --list
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ZEROC = REPO / "build" / "bin" / "zerocc"
TEST_DIR = REPO / "test" / "isa_qemu"

# isa -> zerocc --target value, the static-musl reference wrapper, the QEMU
# runner, and the setup_musl_cross.sh args to (re)build that reference.
ISAS = {
    "x86_64":  dict(target="x86-64",  ref="musl-gcc-x86_64",  qemu="qemu-x86_64-static",
                    setup="--arch x86_64 --ccbin gcc --host x86_64-linux-musl"),
    "aarch64": dict(target="aarch64", ref="musl-gcc-aarch64", qemu="qemu-aarch64-static",
                    setup="--arch aarch64 --ccbin aarch64-linux-gnu-gcc"),
    "riscv64": dict(target="riscv64", ref="musl-gcc-riscv64", qemu="qemu-riscv64-static",
                    setup="--arch riscv64 --ccbin riscv64-linux-gnu-gcc"),
    "riscv32": dict(target="riscv32", ref="musl-gcc-riscv32", qemu="qemu-riscv32-static",
                    setup="--arch riscv32 --ccbin riscv64-linux-gnu-gcc --extra '\''-march=rv32gc -mabi=ilp32'\'' --host riscv32-linux-musl"),
}


def preflight(isas):
    """Return a list of missing tools for the requested ISAs."""
    missing = []
    if not ZEROC.exists():
        missing.append(f"{ZEROC} (run `make build/bin/zerocc` first)")
    for isa in isas:
        cfg = ISAS[isa]
        if shutil.which(cfg["ref"]) is None:
            missing.append(f"{cfg['ref']} (needed for {isa}); build it with "
                           f"tools/setup_musl_cross.sh {cfg['setup']}")
        if shutil.which(cfg["qemu"]) is None:
            missing.append(f"{cfg['qemu']} (needed for {isa})")
    return missing


def run(cmd, env=None, timeout=30, text=False):
    try:
        return subprocess.run(cmd, capture_output=True, text=text,
                              timeout=timeout, env=env)
    except subprocess.TimeoutExpired:
        return None


def build_zerocc(cfg, src, exe, timeout=60):
    """Build a static executable via zerocc's AOT path (defaults to RAW binary emitter; ZC_TEXT_AOT=1 routes via text)."""
    return run([str(ZEROC), "--target", cfg["target"], str(src), "-o", str(exe)],
               env=dict(os.environ), timeout=timeout, text=True)


def build_ref(cfg, src, exe, timeout=60):
    """Build the reference static-musl executable with the musl-gcc-<arch> wrapper."""
    return run([cfg["ref"], "-static", "-O2",
                str(src), "-o", str(exe), "-lc"], timeout=timeout, text=True)


def run_exe(cfg, exe, timeout=15):
    """Run an executable under the matching QEMU; return (stdout, exit, timed_out).
    stdout is RAW BYTES (a broken emitter can emit non-UTF8, so no text decode)."""
    try:
        r = subprocess.run([cfg["qemu"], str(exe)], capture_output=True,
                           timeout=timeout)
        return r.stdout, r.returncode, False
    except subprocess.TimeoutExpired:
        return b"", None, True


def norm(b):
    return (b or b"").strip()


def snip(s):
    return (s or "").strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--isa", dest="isas", nargs="+", default=list(ISAS),
                    choices=list(ISAS), help="ISAs to test (default: all)")
    ap.add_argument("--only", nargs="+", default=None,
                    help="only run test files whose name contains one of these")
    ap.add_argument("--timeout", type=int, default=15, help="per-run timeout (s)")
    ap.add_argument("--text", action="store_true", help="use text assembly + cross-assembler escape hatch (ZC_TEXT_AOT=1)")
    ap.add_argument("--list", action="store_true", help="list test files and exit")
    args = ap.parse_args()

    if args.text:
        os.environ["ZC_TEXT_AOT"] = "1"

    if not ZEROC.exists():
        print(f"[error] {ZEROC} not found -- run `make build/bin/zerocc` first")
        return 1

    tests = sorted(TEST_DIR.glob("*.c"))
    if args.only:
        tests = [t for t in tests if any(k in t.name for k in args.only)]
    if args.list:
        for t in tests:
            print(t.name)
        return 0
    if not tests:
        print(f"[error] no tests in {TEST_DIR}")
        return 1

    missing = preflight(args.isas)
    if missing:
        print("MISSING DEPENDENCIES for QEMU differential testing:\n")
        for m in missing:
            print(f"  - {m}")
        print("\nInstall QEMU (qemu-user-static) and the per-ISA static-musl")
        print("reference via tools/setup_musl_cross.sh (see lines above) and re-run.")
        return 2

    total = pass_ = fail = timeout = buildfail = 0
    work = Path(tempfile.mkdtemp(prefix="isa_qemu_"))

    for isa in args.isas:
        cfg = ISAS[isa]
        s_pass = s_fail = s_to = s_bf = 0
        print(f"\n=== {isa}  (zerocc target={cfg['target']}, "
              f"ref={cfg['ref']} [static musl], run={cfg['qemu']}, RAW binary path) ===")
        for t in tests:
            total += 1
            exe_zc = work / f"{t.stem}_{isa}_zc"
            exe_rf = work / f"{t.stem}_{isa}_ref"

            bz = build_zerocc(cfg, t, exe_zc)
            if bz is None or bz.returncode != 0:
                s_bf += 1; buildfail += 1; total -= 1
                if bz is not None and bz.returncode is not None and bz.returncode < 0:
                    msg = f"zerocc crashed (signal {-bz.returncode})"
                else:
                    msg = snip(bz.stderr.splitlines()[-1] if bz and bz.stderr else '')[:70]
                print(f"  BUILD-FAIL {t.name:<18} ({msg})")
                continue

            br = build_ref(cfg, t, exe_rf)
            if br is None or br.returncode != 0:
                s_bf += 1; buildfail += 1; total -= 1
                print(f"  REF-FAIL   {t.name:<18} "
                      f"({snip(br.stderr.splitlines()[-1] if br and br.stderr else '')[:70]})")
                continue

            zo, zrc, zto = run_exe(cfg, exe_zc, args.timeout)
            ro, rrc, rto = run_exe(cfg, exe_rf, args.timeout)
            if zto or rto:
                s_to += 1; timeout += 1; total -= 1
                print(f"  TIMEOUT    {t.name:<18} (zc_timeout={zto} ref_timeout={rto})")
                continue

            if norm(zo) == norm(ro) and zrc == rrc:
                s_pass += 1; pass_ += 1
                print(f"  ok         {t.name:<18} rc={zrc}")
            else:
                s_fail += 1; fail += 1
                zd = norm(zo)[:60].decode("utf-8", "replace")
                rd = norm(ro)[:60].decode("utf-8", "replace")
                print(f"  MISMATCH   {t.name:<18} zc(rc={zrc})=<{zd}>  "
                      f"ref(rc={rrc})=<{rd}>")

        n = s_pass + s_fail + s_to + s_bf
        print(f"  [{isa}] {s_pass} ok, {s_fail} mismatch, {s_to} timeout, {s_bf} build-fail  ({n} tests)")

    print(f"\n=== TOTAL: {pass_} ok, {fail} mismatch, {timeout} timeout, "
          f"{buildfail} build-fail  ({total} isa-tests) ===")
    shutil.rmtree(work, ignore_errors=True)
    return 0 if (fail == 0 and timeout == 0) else 1


if __name__ == "__main__":
    sys.exit(main())
