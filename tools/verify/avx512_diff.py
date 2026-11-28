#!/usr/bin/env python3
"""AVX-512 Encoding & Execution Differential Gate (Sprint 277).

QEMU user-mode (TCG) cannot execute AVX-512, and many dev hosts lack it, so
this gate does NOT depend on running EVEX code.  It proves the two emitters
agree and that the bytes are valid x86, with independent tools as oracles:

  1. raw  : `zerocc -c` (in-process EVEX byte emitter)  -> `objdump -d`
  2. text : `zerocc -S` (AT&T text) -> `gcc-15 -c` (GNU as) -> `objdump -d`
  3. Every EVEX-bearing line (zmm / ymm16-31 / xmm16-31 / %k0-7 / {%kN} /
     {1toN} / {sae}) disassembled from the raw object must match the
     disassembly of the GNU-as object, in order, at -O0..-O3.
  4. The raw disassembly must contain no `(bad)` decode.
  5. Coverage: the union of mnemonics must include every mnemonic listed in
     REQUIRED_MNEMONICS (guards against silently lowering to the fallback).
  6. If the host really has AVX-512 (/proc/cpuinfo avx512f) the executable
     is also run and diffed (stdout + rc) against a gcc-15 -O2 reference;
     otherwise execution is reported as skipped.

Usage:
    python3 tools/verify/avx512_diff.py
    python3 tools/verify/avx512_diff.py --levels 0 2 --only mask
    python3 tools/verify/avx512_diff.py --list
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
from tools import find_repo_root
import argparse
import re
import shutil
import subprocess
import tempfile

REPO = find_repo_root()
ZEROC = REPO / "build" / "bin" / "zerocc"
TEST_DIR = REPO / "test" / "jit"
TEST_GLOB = "avx512_*.c"
FLAGS = ["-mavx512f", "-mavx512bw", "-mavx512dq", "-mavx512vl", "-mavx512cd"]
GCC_FLAGS = ["-mavx512f", "-mavx512bw", "-mavx512dq", "-mavx512vl", "-mavx512cd"]
LEVELS = [0, 1, 2, 3]

EVEX_RE = re.compile(
    r"zmm|[xy]mm(1[6-9]|2[0-9]|3[01])|%k[0-7]|\{1to\d+\}|\{r[nduz]-sae\}|\{sae\}")

# Mnemonics that MUST appear (across the whole corpus) in the raw disassembly.
REQUIRED_MNEMONICS = {
    "vaddps", "vsubps", "vmulps", "vdivps", "vaddpd", "vmovups",
    "vpaddd", "kmovw", "kandw", "korw",
    "vpminsd", "vpmaxsd", "vpminud", "vpmaxud", "vpermd", "vpermps",
}


def have_avx512_hw():
    try:
        return " avx512f" in Path("/proc/cpuinfo").read_text()
    except OSError:
        return False


def run(cmd, timeout=120):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(cmd, 124, "", "timeout")


def disasm_evex(obj):
    """Return (evex_lines, bad_count, mnemonics) from objdump -d."""
    r = run(["objdump", "-d", "--no-show-raw-insn", str(obj)])
    lines, bad, mn = [], 0, set()
    for ln in r.stdout.splitlines():
        if "(bad)" in ln:
            bad += 1
        m = re.match(r"^\s*[0-9a-f]+:\s*(.*)$", ln)
        if not m:
            continue
        ins = re.sub(r"\s+", " ", m.group(1).split("#")[0]).strip()
        if EVEX_RE.search(ins) or ins.startswith("k"):
            lines.append(ins)
            mn.add(ins.split(" ")[0])
    return lines, bad, mn


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--levels", type=int, nargs="*", default=LEVELS)
    ap.add_argument("--only", default="")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()

    tests = sorted(p for p in TEST_DIR.glob(TEST_GLOB) if a.only in p.name)
    if a.list:
        for t in tests:
            print(t.name)
        return 0
    miss = [x for x in (str(ZEROC), shutil.which("gcc-15"), shutil.which("objdump")) if not x or not Path(x).exists()]
    if miss:
        print("avx512_diff: missing tools:", miss, file=sys.stderr)
        return 2

    hw = have_avx512_hw()
    fails, total, all_mn = 0, 0, set()
    print(f"AVX-512 gate: {len(tests)} tests x {len(a.levels)} levels | "
          f"hardware exec: {'yes' if hw else 'skipped (no AVX-512 host; TCG lacks it)'}")
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        for t in tests:
            for lv in a.levels:
                total += 1
                tag = f"{t.name} -O{lv}"
                raw, s, txt = td / "raw.o", td / "t.s", td / "txt.o"
                r1 = run([str(ZEROC), f"-O{lv}", *FLAGS, "-c", str(t), "-o", str(raw)])
                r2 = run([str(ZEROC), f"-O{lv}", *FLAGS, "-S", str(t), "-o", str(s)])
                if r1.returncode or r2.returncode:
                    print(f"  FAIL {tag}: zerocc rc raw={r1.returncode} text={r2.returncode} "
                          f"{(r1.stderr or r2.stderr)[:160]}")
                    fails += 1
                    continue
                r3 = run(["gcc-15", "-c", str(s), "-o", str(txt)])
                if r3.returncode:
                    print(f"  FAIL {tag}: GNU as rejected -S output: {r3.stderr[:200]}")
                    fails += 1
                    continue
                la, bad, mn = disasm_evex(raw)
                lb, _, _ = disasm_evex(txt)
                all_mn |= mn
                if bad:
                    print(f"  FAIL {tag}: {bad} (bad) decode(s) in raw object")
                    fails += 1
                    continue
                if la != lb:
                    d = next((i for i, (x, y) in enumerate(zip(la, lb)) if x != y), min(len(la), len(lb)))
                    print(f"  FAIL {tag}: raw/text EVEX mismatch at #{d} "
                          f"(raw={la[d] if d < len(la) else '<end>'!r} "
                          f"text={lb[d] if d < len(lb) else '<end>'!r}; {len(la)} vs {len(lb)} lines)")
                    fails += 1
                    continue
                if hw:
                    exe, ref = td / "z.exe", td / "g.exe"
                    run([str(ZEROC), f"-O{lv}", *FLAGS, str(t), "-o", str(exe)])
                    run(["gcc-15", "-O2", *GCC_FLAGS, "-static", str(t), "-o", str(ref)])
                    ra, rb = run([str(exe)]), run([str(ref)])
                    if (ra.returncode, ra.stdout) != (rb.returncode, rb.stdout):
                        print(f"  FAIL {tag}: exec diff rc {ra.returncode} vs {rb.returncode}")
                        fails += 1
                        continue
                print(f"  ok   {tag} ({len(la)} EVEX insns)")
    missing = sorted(REQUIRED_MNEMONICS - all_mn)
    if missing:
        print(f"  FAIL coverage: required mnemonics never emitted: {missing}")
        fails += 1
    print(f"AVX-512 gate: {total - fails if not missing else total - fails}/{total} runs ok, "
          f"{len(all_mn)} distinct EVEX/mask mnemonics, {fails} FAIL")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
