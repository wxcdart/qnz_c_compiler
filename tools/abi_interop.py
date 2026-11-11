#!/usr/bin/env python3
"""
abi_interop.py - external ABI interop gate (Sprint 158, plan item #4/#5)

Differential-tests ZeroCC's System V AMD64 ABI **against real GCC** by
linking zerocc-compiled objects with gcc-compiled objects in one binary
(x86-64 only - the other ISAs have no host-gcc interop oracle):

  Each scenario in test/abi_interop/ is a triple:
    <name>.c       shared main (compiled by BOTH compilers)
    <name>_zc.c    provider compiled by ZEROCC
    <name>_gc.c    provider compiled by GCC
  and is run in four ways per zerocc -O level:

    control   all three files with gcc -O2          (validates the
              scenario itself + the ratcheted `expect:` output)
    A         zerocc main + zerocc provider + gcc provider
              (zerocc caller -> gcc callee)
    B         gcc main + zerocc provider + gcc provider
              (gcc caller -> zerocc callee)

  All four must produce identical stdout + exit code.  Linking is done
  by the SYSTEM gcc (-static) so the interop surface is the object
  files' ABI - calling convention, struct/sret passing, data layout -
  not zerocc's linker.

Why this gate exists: every other gate is self-differential (zerocc
vs zerocc, or zerocc vs gcc behavior of the same translation unit).
A SYSTEMATIC ABI misunderstanding - e.g. passing >16B by-value struct
arguments as hidden pointers (zerocc's pre-Sprint-158 model) instead
of the System V memory-argument convention - is invisible to
self-differential testing because both sides share the mistake; it
only shows up when a zerocc .o meets a gcc .o.  This gate is the
ground-truth check for the external ABI and the enabling verification
for the self-hosting epic.

The `// expect:` lines in each shared main ratchet the reference
output (a scenario bug that silently changes the expected values is
caught even if all four builds agree).

Reference compiler: ABI_INTEROP_GCC env override, else gcc-15, else
gcc-14, else gcc.

Usage:
    python3 tools/abi_interop.py [--zerocc PATH] [--levels 0 1 2]
                                 [--verbose] [--scenario NAME]
"""

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORPUS_DIR = os.path.join(ROOT, "test", "abi_interop")

ZEROCC = os.environ.get("ZEROCC", os.path.join(ROOT, "build", "bin", "zerocc"))
GCC = os.environ.get("ABI_INTEROP_GCC") or next(
    (c for c in ("gcc-15", "gcc-14", "gcc") if shutil.which(c)), "gcc")

EXPECT_RE = re.compile(r"^\s*\*\s*expect:\s*(.*)$", re.MULTILINE)

SKIP_LINK_LIBS = ["-lm"]  # scenarios may use math.h; -lm is harmless otherwise


def run(cmd, timeout=30):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return "timeout", "", ""


def scenarios():
    """Yield (name, main, zc, gc) for every complete scenario triple."""
    out = []
    for main in sorted(glob.glob(os.path.join(CORPUS_DIR, "s*.c"))):
        base = os.path.splitext(main)[0]
        name = os.path.basename(base)
        zc = base + "_zc.c"
        gc = base + "_gc.c"
        if not (os.path.exists(zc) and os.path.exists(gc)):
            continue
        out.append((name, main, zc, gc))
    return out


def expected_lines(main_path):
    src = open(main_path).read()
    return [m.group(1).rstrip() for m in EXPECT_RE.finditer(src)]


def compile_obj(compiler, extra, src, out):
    rc, so, se = run([compiler] + extra + [src, "-c", "-o", out], timeout=60)
    if rc != 0:
        return False, se
    return True, ""


def build_and_run(workdir, objs, exe, timeout=15):
    exe_path = os.path.join(workdir, exe)
    rc, so, se = run([GCC, "-static"] + objs + ["-o", exe_path] + SKIP_LINK_LIBS, timeout=60)
    if rc != 0:
        return "link-fail", se
    rc, so, se = run([exe_path], timeout=timeout)
    if rc == "timeout":
        return "timeout", ""
    return (rc, so), ""


def check_scenario(name, main, zc, gc, levels, verbose, zerocc=ZEROCC, gcc=GCC):
    """Run one scenario. Returns (ok, detail_lines)."""
    detail = []
    exp = expected_lines(main)

    with tempfile.TemporaryDirectory(prefix="abi_interop_") as wd:
        # --- gcc-only control (scenario validation + reference) ---
        ok, err = compile_obj(gcc, ["-O2"], main, f"{wd}/ctl_main.o")
        if not ok:
            return False, [f"control: gcc main compile failed: {err.strip()}"]
        ok, err = compile_obj(gcc, ["-O2"], zc, f"{wd}/ctl_zc.o")
        if not ok:
            return False, [f"control: gcc zc compile failed: {err.strip()}"]
        ok, err = compile_obj(gcc, ["-O2"], gc, f"{wd}/ctl_gc.o")
        if not ok:
            return False, [f"control: gcc gc compile failed: {err.strip()}"]
        res, err = build_and_run(wd, [f"{wd}/ctl_main.o", f"{wd}/ctl_zc.o",
                                      f"{wd}/ctl_gc.o"], "ctl")
        if not isinstance(res, tuple):
            return False, [f"control: {res} ({err.strip()[:200]})"]
        crc, cout = res
        if crc != 0:
            return False, [f"control: gcc-only run rc={crc} (scenario itself is broken)"]
        ref = cout
        if exp is not None and exp != []:
            got = [l.rstrip() for l in cout.splitlines()]
            if got != exp:
                return False, [f"control: output differs from ratcheted expect lines:\n"
                               f"  expect: {exp}\n  got:    {got}"]
        # compile the gcc-side objects once (shared by all zerocc runs)
        ok, err = compile_obj(gcc, ["-O2"], gc, f"{wd}/gc.o")
        if not ok:
            return False, [f"gc provider compile failed: {err.strip()}"]

        # --- direction A: zerocc main + zerocc provider ---
        for o in levels:
            tag = f"A-O{o}"
            ok, err = compile_obj(zerocc, [f"-O{o}"], main, f"{wd}/A_main.o")
            if not ok:
                detail.append(f"{tag}: zerocc main compile failed: {err.strip()[:200]}")
                continue
            ok, err = compile_obj(zerocc, [f"-O{o}"], zc, f"{wd}/A_zc.o")
            if not ok:
                detail.append(f"{tag}: zerocc zc compile failed: {err.strip()[:200]}")
                continue
            res, err = build_and_run(wd, [f"{wd}/A_main.o", f"{wd}/A_zc.o", f"{wd}/gc.o"],
                                     f"A_{o}")
            if not isinstance(res, tuple):
                detail.append(f"{tag}: {res} ({err.strip()[:200]})")
                continue
            rc, out = res
            if rc != 0 or out != ref:
                detail.append(f"{tag}: rc={rc} (want {crc})"
                              + (f"\n  got:  {out!r}" if out != ref else ""))
            elif verbose:
                detail.append(f"{tag}: ok")

        # --- direction B: gcc main + zerocc provider ---
        ok, err = compile_obj(gcc, ["-O2"], main, f"{wd}/B_main.o")
        if not ok:
            detail.append("B: gcc main compile failed: " + err.strip()[:200])
        else:
            for o in levels:
                tag = f"B-O{o}"
                ok, err = compile_obj(zerocc, [f"-O{o}"], zc, f"{wd}/B_zc.o")
                if not ok:
                    detail.append(f"{tag}: zerocc zc compile failed: {err.strip()[:200]}")
                    continue
                res, err = build_and_run(wd, [f"{wd}/B_main.o", f"{wd}/B_zc.o", f"{wd}/gc.o"],
                                         f"B_{o}")
                if not isinstance(res, tuple):
                    detail.append(f"{tag}: {res} ({err.strip()[:200]})")
                    continue
                rc, out = res
                if rc != 0 or out != ref:
                    detail.append(f"{tag}: rc={rc} (want {crc})"
                                  + (f"\n  got:  {out!r}" if out != ref else ""))
                elif verbose:
                    detail.append(f"{tag}: ok")
    bad = [d for d in detail if not d.endswith(": ok")]
    return not bad, detail


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--zerocc", default=ZEROCC)
    ap.add_argument("--gcc", default=GCC)
    ap.add_argument("--levels", type=int, nargs="+", default=[0, 1, 2])
    ap.add_argument("--scenario", help="run a single scenario (name prefix match)")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.zerocc):
        print(f"abi_interop: zerocc not found at {args.zerocc} (build it first)",
              file=sys.stderr)
        return 2
    if not os.path.isdir(CORPUS_DIR):
        print(f"abi_interop: corpus dir missing: {CORPUS_DIR}", file=sys.stderr)
        return 2

    scens = scenarios()
    if args.scenario:
        scens = [s for s in scens if s[0].startswith(args.scenario)]
        if not scens:
            print(f"abi_interop: no scenario matches {args.scenario!r}", file=sys.stderr)
            return 2

    print(f"ABI interop gate: zerocc={args.zerocc} ref-cc={args.gcc} "
          f"levels={args.levels} scenarios={len(scens)}")
    ok_count = 0
    for name, main, zc, gc in scens:
        ok, detail = check_scenario(name, main, zc, gc, args.levels, args.verbose,
                                    args.zerocc, args.gcc)
        status = "ok" if ok else "FAIL"
        print(f"  [{status}] {name}")
        for d in detail:
            if ok and not args.verbose:
                continue
            print(f"          {d}")
        if ok:
            ok_count += 1

    print("=" * 60)
    print(f"abi_interop: {ok_count}/{len(scens)} scenarios ok "
          f"(each: gcc control + zerocc A/B at -O{args.levels})")
    return 0 if ok_count == len(scens) else 1


if __name__ == "__main__":
    sys.exit(main())
