#!/usr/bin/env python3
"""Cross-ISA __int128 Differential Gate (Sprint 109).

Compiles the `test/jit/int128_*.c` suite with zerocc for each cross target
ISA at -O0..-O3, in BOTH AOT codegen paths:

  * raw    -- `zerocc -O<level> --target <isa> t.c -o exe`
  * text   -- `zerocc -O<level> --target <isa> -S t.c -o t.s` followed by the
              system cross-assembler + static link

...runs each executable under `qemu-*-static`, and diffs (stdout, exit code)
against a host `gcc -static -O2` reference build of the same source.

Why this gate exists: every Sprint 109 128-bit bug (RISC-V `t5`/AArch64
`x16` large-offset scratch clobbers, the multiword borrow `sltu` ordering,
all-spilled MUL128, mul/umulh ordering, 128-bit call-return high-half
protection) was CROSS-ISA only -- invisible to `make test` (host gcc) and
`make test-opt` (x86-64 only).  This harness is the permanent differential
guard for the 128-bit (lo,hi) GPR128 value model and its emitters on
aarch64 / riscv64, across optimization levels and both AOT paths.

Dependencies (required):
  - build/bin/zerocc (make build/bin/zerocc)
  - qemu-aarch64-static, qemu-riscv64-static (qemu-user-static)
  - aarch64-linux-gnu-gcc, riscv64-linux-gnu-gcc (cross toolchains)
  - host gcc (reference builds)

Usage:
    python3 tools/isa128_diff.py                     # all ISAs, all levels
    python3 tools/isa128_diff.py --isa aarch64       # one ISA
    python3 tools/isa128_diff.py --levels 0 2        # subset of O levels
    python3 tools/isa128_diff.py --only arith        # subset by name
    python3 tools/isa128_diff.py --list
"""
import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ZEROC = REPO / "build" / "bin" / "zerocc"
TEST_DIR = REPO / "test" / "jit"
TEST_GLOB = "int128_*.c"

# isa -> zerocc --target value, the QEMU runner, and the cross linker used
# for the -S path (same flags the driver's system-link step uses).
# x86_64 is included as a native (no-QEMU) baseline producer/verifier.
ISAS = {
    "x86_64":  dict(target="x86-64", qemu=None,
                    link="gcc -static -no-pie", run_native=True),
    "aarch64": dict(target="aarch64", qemu="qemu-aarch64-static",
                    link="aarch64-linux-gnu-gcc -mabi=lp64 -march=armv8-a -static",
                    run_native=False),
    "riscv64": dict(target="riscv64", qemu="qemu-riscv64-static",
                    link="riscv64-linux-gnu-gcc -mabi=lp64d -march=rv64gc -static",
                    run_native=False),
}
LEVELS = [0, 1, 2, 3]
PATHS = ["aot", "asm"]


def preflight(isas):
    """Return a list of missing tools for the requested ISAs."""
    missing = []
    if not ZEROC.exists():
        missing.append(f"{ZEROC} (run `make build/bin/zerocc` first)")
    if shutil.which("gcc") is None:
        missing.append("gcc (needed for reference builds)")
    for isa in isas:
        cfg = ISAS[isa]
        if not cfg.get("run_native") and shutil.which(cfg["qemu"]) is None:
            missing.append(f"{cfg['qemu']} (needed for {isa}; install qemu-user-static)")
        linker = cfg["link"].split()[0]
        if shutil.which(linker) is None:
            missing.append(f"{linker} (needed for {isa} -S-path linking)")
    return missing


def run(cmd, env=None, timeout=60):
    try:
        return subprocess.run(cmd, capture_output=True, text=False,
                              timeout=timeout, env=env)
    except subprocess.TimeoutExpired:
        return None


def last_err(r):
    if r is None:
        return "timeout"
    try:
        lines = r.stderr.decode("utf-8", "replace").strip().splitlines()
        return lines[-1][:80] if lines else f"rc={r.returncode}"
    except Exception:
        return "unknown"


def build_aot(cfg, level, src, exe, work):
    """Raw-AOT path: zerocc compiles and links in one step."""
    return run([str(ZEROC), f"-O{level}", "--target", cfg["target"],
                str(src), "-o", str(exe)])


def build_asm(cfg, level, src, exe, work):
    """Text-AOT path: zerocc -S, then cross-assemble + static link."""
    s = work / (exe.name + ".s")
    r = run([str(ZEROC), f"-O{level}", "--target", cfg["target"], "-S",
             str(src), "-o", str(s)])
    if r is None or r.returncode != 0:
        return r
    link = cfg["link"].split()
    return run(link + ["-Wl,--defsym=link_error=0", "-o", str(exe),
                       str(s), "-lc", "-lm"])


def build_ref(src, exe):
    """Host gcc reference (stronger than a zerocc-x86 baseline: validates
    the expected constants against a known-good 128-bit implementation)."""
    return run(["gcc", "-static", "-O2", str(src), "-o", str(exe)])


def run_exe(cfg, exe, timeout=15):
    """Run an executable (under QEMU for cross targets); return
    (stdout_bytes, exit, timed_out)."""
    cmd = ([cfg["qemu"]] if cfg.get("qemu") else []) + [str(exe)]
    try:
        r = subprocess.run(cmd, capture_output=True, timeout=timeout)
        return r.stdout, r.returncode, False
    except subprocess.TimeoutExpired:
        return b"", None, True


def norm(b):
    return (b or b"").strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--isa", dest="isas", nargs="+", default=list(ISAS),
                    choices=list(ISAS), help="ISAs to test (default: all)")
    ap.add_argument("--levels", type=int, nargs="+", default=LEVELS,
                    choices=LEVELS, help="optimization levels (default: 0 1 2 3)")
    ap.add_argument("--paths", nargs="+", default=PATHS, choices=PATHS,
                    help="AOT paths to test (default: aot asm)")
    ap.add_argument("--only", nargs="+", default=None,
                    help="only run test files whose name contains one of these")
    ap.add_argument("--timeout", type=int, default=15, help="per-run timeout (s)")
    ap.add_argument("--list", action="store_true", help="list test files and exit")
    args = ap.parse_args()

    tests = sorted(TEST_DIR.glob(TEST_GLOB))
    if args.only:
        tests = [t for t in tests if any(k in t.name for k in args.only)]
    if args.list:
        for t in tests:
            print(t.name)
        return 0
    if not tests:
        print(f"[error] no {TEST_GLOB} tests in {TEST_DIR}")
        return 1

    missing = preflight(args.isas)
    if missing:
        print("MISSING DEPENDENCIES for the cross-ISA 128-bit gate:\n")
        for m in missing:
            print(f"  - {m}")
        print("\nInstall qemu-user-static and the cross toolchains,")
        print("build zerocc (make build/bin/zerocc), and re-run.")
        return 2

    work = Path(tempfile.mkdtemp(prefix="isa128_"))
    total = ok = fail = timeout = buildfail = referr = 0

    # Reference: one host gcc build per test.
    refs = {}
    for t in tests:
        ref = work / f"ref_{t.stem}"
        r = build_ref(t, ref)
        if r is None or r.returncode != 0:
            print(f"  REF-FAIL   {t.name:<20} ({last_err(r)})")
            referr += 1
            continue
        out, rc, to = run_exe(ISAS["x86_64"], ref, args.timeout)
        if to or rc != 0:
            print(f"  REF-FAIL   {t.name:<20} (reference rc={rc})")
            referr += 1
            continue
        refs[t.name] = (norm(out), rc)
        print(f"  ref ok     {t.name:<20} (gcc -static -O2)")

    for isa in args.isas:
        cfg = ISAS[isa]
        s_ok = s_fail = s_to = s_bf = 0
        mode = "native" if cfg.get("run_native") else cfg["qemu"]
        print(f"\n=== {isa}  (zerocc target={cfg['target']}, run={mode}) ===")
        for t in tests:
            if t.name not in refs:
                continue
            ref_out, ref_rc = refs[t.name]
            for level in args.levels:
                for path in args.paths:
                    label = f"{t.stem:<16} -O{level} {path}"
                    exe = work / f"{t.stem}_{isa}_{path}_O{level}"
                    total += 1
                    r = (build_aot if path == "aot" else build_asm)(
                        cfg, level, t, exe, work)
                    if r is None or r.returncode != 0:
                        s_bf += 1; buildfail += 1
                        print(f"  BUILD-FAIL {label} ({last_err(r)})")
                        continue
                    out, rc, to = run_exe(cfg, exe, args.timeout)
                    if to:
                        s_to += 1; timeout += 1
                        print(f"  TIMEOUT    {label}")
                        continue
                    if norm(out) == ref_out and rc == ref_rc:
                        s_ok += 1; ok += 1
                        print(f"  ok         {label}")
                    else:
                        s_fail += 1; fail += 1
                        zo = norm(out)[:50].decode("utf-8", "replace")
                        ro = ref_out[:50].decode("utf-8", "replace")
                        print(f"  MISMATCH   {label} zc(rc={rc})=<{zo}> "
                              f"ref(rc={ref_rc})=<{ro}>")
        n = s_ok + s_fail + s_to + s_bf
        print(f"  [{isa}] {s_ok} ok, {s_fail} mismatch, {s_to} timeout, "
              f"{s_bf} build-fail  ({n} runs)")

    print(f"\n=== TOTAL: {ok} ok, {fail} mismatch, {timeout} timeout, "
          f"{buildfail} build-fail  ({total} isa-runs; {len(refs)} refs) ===")
    shutil.rmtree(work, ignore_errors=True)
    return 0 if (fail == 0 and timeout == 0 and buildfail == 0 and referr == 0) else 1


if __name__ == "__main__":
    sys.exit(main())
