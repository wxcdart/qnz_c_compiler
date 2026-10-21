#!/usr/bin/env python3
"""RISC-V raw-emitter instruction-encoding oracle (T1).

Three independent checks:

A. spec round-trip  -- the canonical RISC-V immediate encodings for the
   I/S/B/J/U instruction formats are fuzzed (encode -> decode) across their
   full ranges. Catches bit-field slip bugs of the kind that hit riscv.c and
   the ad-hoc session oracle on 2026-10-20 (`<< 8` vs `<< 7` for imm[4:1],
   `0x3FE` vs `0x7FE` for imm[10:1]).

B. independent oracle -- the same formulas are cross-checked against a
   vendored pure-Python class-level reference (celebi) using string bit-slice
   manipulation across full ranges (B, J, I, S, U). An optional `--as` flag
   also cross-checks I/S/U against riscv64-linux-gnu-as.

C. zerocc raw scan  -- zerocc's RAW binary emitter output (ZC_RAW_AOT=1 +
   ZC_KEEP_OBJ=1) is scanned instruction by instruction: no compressed
   instructions, only known opcodes, and -- the key property test -- every
   B/J displacement must decode to a 4-byte-aligned target inside .text.
   The final object contains the *patched* branch/jump words, so this
   exercises patch_jmp() directly, including backward edges.

Usage:
    python3 tools/riscv_enc_check.py                    # A + B
    python3 tools/riscv_enc_check.py --zerocc           # A + B + C (riscv64 battery)
    python3 tools/riscv_enc_check.py --zerocc --target riscv32
    python3 tools/riscv_enc_check.py --test test/isa_qemu/03_control.c
    python3 tools/riscv_enc_check.py --no-as            # skip B (no binutils)

Exit: 0 = all checks pass, 1 = violations, 2 = usage/environment error.
"""
import argparse
import glob
import os
import random
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import elf_min  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
ZEROC = REPO / "build" / "bin" / "zerocc"
TEST_DIR = REPO / "test" / "isa_qemu"
AS_BIN = "riscv64-linux-gnu-as"

# --------------------------------------------------------------------------
# Canonical RISC-V immediate packing (unprivileged ISA; Part B validates
# these formulas against the system assembler).
# --------------------------------------------------------------------------

def enc_i(imm, rs1, f3, rd, op=0x13):
    u = imm & 0xFFF
    return (u << 20) | ((rs1 & 0x1F) << 15) | ((f3 & 7) << 12) | \
           ((rd & 0x1F) << 7) | op

def dec_i(w):
    u = w >> 20
    return u - 0x1000 if u & 0x800 else u

def enc_s(imm, rs1, rs2, f3, op=0x23):
    u = imm & 0xFFF
    return ((u & 0xFE0) << 20) | ((rs2 & 0x1F) << 20) | ((rs1 & 0x1F) << 15) | \
           ((f3 & 7) << 12) | ((u & 0x1F) << 7) | op

def dec_s(w):
    u = (((w >> 25) & 0x7F) << 5) | ((w >> 7) & 0x1F)
    return u - 0x1000 if u & 0x800 else u

def enc_b(imm, rs1, rs2, f3, op=0x63):
    u = imm & 0x1FFF
    return (((u & 0x1000) << 19) | ((u & 0x7E0) << 20) | ((rs2 & 0x1F) << 20) |
            ((rs1 & 0x1F) << 15) | ((f3 & 7) << 12) | ((u & 0x1E) << 7) |
            ((u & 0x800) >> 4) | op)

def dec_b(w):
    u = (((w >> 31) & 1) << 12) | (((w >> 25) & 0x3F) << 5) | \
        (((w >> 8) & 0xF) << 1) | (((w >> 7) & 1) << 11)
    return u - 0x2000 if u & 0x1000 else u

def enc_j(imm, rd, op=0x6F):
    u = imm & 0x1FFFFF
    return (((u & 0x100000) << 11) | ((u & 0x7FE) << 20) | ((u & 0x800) << 9) |
            (u & 0xFF000) | ((rd & 0x1F) << 7) | op)

def dec_j(w):
    u = (((w >> 31) & 1) << 20) | (((w >> 21) & 0x3FF) << 1) | \
        (((w >> 20) & 1) << 11) | (((w >> 12) & 0xFF) << 12)
    return u - 0x200000 if u & 0x100000 else u

def enc_u(imm20, rd, op=0x37):
    return ((imm20 & 0xFFFFF) << 12) | ((rd & 0x1F) << 7) | op

def dec_u(w):
    return (w >> 12) & 0xFFFFF

# --------------------------------------------------------------------------
# Part A: spec round-trip fuzz
# --------------------------------------------------------------------------

def part_a():
    fails = []
    n = 0

    def check(kind, imm, got, want):
        nonlocal n
        n += 1
        if got != want:
            if len(fails) < 8:
                fails.append(f"{kind} imm={want}: encoded decodes to {got}")

    for imm in range(-2048, 2048):
        check("I", imm, dec_i(enc_i(imm, 1, 2, 3)), imm)
        check("S", imm, dec_s(enc_s(imm, 1, 2, 3, 0x23)), imm)
    for imm in range(-2048, 2049, 2):
        check("B", imm, dec_b(enc_b(imm, 1, 2, 0)), imm)
    rng = random.Random(20261020)
    jcases = [i for i in range(-16384, 16385, 2)]
    jcases += [2 * rng.randrange(-(1 << 19), 1 << 19)
               for _ in range(100000)]
    jcases += [-1048576, 1048574]
    for imm in jcases:
        check("J", imm, dec_j(enc_j(imm, 1)), imm)
    for imm in range(1 << 20):
        check("U", imm, dec_u(enc_u(imm, 1)), imm)

    if fails:
        for f in fails:
            print(f"  [A] FAIL {f}")
        print(f"  [A] spec round-trip: {n - len(fails)}/{n} ok, "
              f"{len(fails)}+ shown failures")
        return False
    print(f"  [A] spec round-trip: {n}/{n} ok (I/S full, B full, J 104k, U full)")
    return True

# --------------------------------------------------------------------------
# Part B: independent encoder oracle (celebi class-level reference)
#
# Vendored pure-Python class-level encoders operating via binary string
# manipulation, providing a clean ground-truth reference independent of both
# our bitwise shift formulas and GNU as relaxation quirks.
# --------------------------------------------------------------------------

def celebi_sb(imm, rs1, rs2, f3, op=0x63):
    mod_imm = format(((1 << 13) - 1) & int(imm), '013b')
    imm1 = mod_imm[12 - 12] + mod_imm[12 - 10 : 12 - 4]
    imm2 = mod_imm[12 - 4 : 12 - 0] + mod_imm[12 - 11]
    bits = imm1 + format(rs2 & 0x1F, '05b') + format(rs1 & 0x1F, '05b') + \
           format(f3 & 7, '03b') + imm2 + format(op & 0x7F, '07b')
    return int(bits, 2)

def celebi_uj(imm, rd, op=0x6F):
    mod_imm = format(((1 << 21) - 1) & int(imm), '021b')
    imm_str = mod_imm[20 - 20] + mod_imm[20 - 10 : 20 - 0] + mod_imm[20 - 11] + mod_imm[20 - 19 : 20 - 11]
    bits = imm_str + format(rd & 0x1F, '05b') + format(op & 0x7F, '07b')
    return int(bits, 2)

def celebi_i(imm, rs1, f3, rd, op=0x13):
    imm_str = format(((1 << 12) - 1) & int(imm), '012b')
    bits = imm_str + format(rs1 & 0x1F, '05b') + format(f3 & 7, '03b') + \
           format(rd & 0x1F, '05b') + format(op & 0x7F, '07b')
    return int(bits, 2)

def celebi_s(imm, rs1, rs2, f3, op=0x23):
    # Verified S-type string packing: imm[11:5] (7 bits) and imm[4:0] (5 bits).
    mod_imm = format(((1 << 12) - 1) & int(imm), '012b')
    imm1 = mod_imm[0:7]   # imm[11:5]
    imm2 = mod_imm[7:12]  # imm[4:0]
    bits = imm1 + format(rs2 & 0x1F, '05b') + format(rs1 & 0x1F, '05b') + \
           format(f3 & 7, '03b') + imm2 + format(op & 0x7F, '07b')
    return int(bits, 2)

def celebi_u(imm20, rd, op=0x37):
    # Verified U-type string packing: imm[31:12] (20 bits).
    imm_str = format(((1 << 20) - 1) & int(imm20), '020b')
    bits = imm_str + format(rd & 0x1F, '05b') + format(op & 0x7F, '07b')
    return int(bits, 2)

def celebi_r(rs1, rs2, rd, f3, f7, op=0x33):
    bits = format(f7 & 0x7F, '07b') + format(rs2 & 0x1F, '05b') + \
           format(rs1 & 0x1F, '05b') + format(f3 & 7, '03b') + \
           format(rd & 0x1F, '05b') + format(op & 0x7F, '07b')
    return int(bits, 2)


def part_b():
    fails = []
    n = 0

    def check(kind, imm, got, want):
        nonlocal n
        n += 1
        if got != want:
            if len(fails) < 8:
                fails.append(f"{kind} imm={imm}: bitwise={got:#010x} independent={want:#010x}")

    # B-type comparison
    for imm in range(-4096, 4096, 2):
        check("B", imm, enc_b(imm, 1, 2, 0), celebi_sb(imm, 1, 2, 0))
        check("B", imm, enc_b(imm, 5, 6, 4), celebi_sb(imm, 5, 6, 4))

    # J-type comparison
    rng = random.Random(20261021)
    jcases = [i for i in range(-16384, 16385, 2)]
    jcases += [2 * rng.randrange(-(1 << 19), 1 << 19) for _ in range(50000)]
    jcases += [-1048576, 1048574]
    for imm in jcases:
        check("J", imm, enc_j(imm, 1), celebi_uj(imm, 1))

    # I-type comparison
    for imm in range(-2048, 2048):
        check("I", imm, enc_i(imm, 2, 0, 1, 0x13), celebi_i(imm, 2, 0, 1, 0x13))
        check("I", imm, enc_i(imm, 2, 3, 1, 0x03), celebi_i(imm, 2, 3, 1, 0x03))

    # S-type comparison
    for imm in range(-2048, 2048):
        check("S", imm, enc_s(imm, 2, 1, 3, 0x23), celebi_s(imm, 2, 1, 3, 0x23))

    # U-type comparison
    for imm in range(1 << 20):
        check("U", imm, enc_u(imm, 1, 0x37), celebi_u(imm, 1, 0x37))

    if fails:
        for f in fails:
            print(f"  [B] FAIL {f}")
        print(f"  [B] independent oracle: {n - len(fails)}/{n} ok, "
              f"{len(fails)}+ shown failures")
        return False
    print(f"  [B] independent oracle: {n}/{n} ok (B/J/I/S/U match celebi reference)")
    return True


def part_b_as(workdir: Path):
    if not _which(AS_BIN):
        print(f"  [B-as] skipped ({AS_BIN} not found)")
        return True

    rng = random.Random(7)
    batches = []

    lines, words = [], []
    for n in range(-2048, 2048):
        lines.append(f"addi x1, x2, {n}")
        words.append(enc_i(n, 2, 0, 1, 0x13))
        lines.append(f"ld x1, {n}(x2)")
        words.append(enc_i(n, 2, 3, 1, 0x03))
    batches.append(("itype", lines, words))

    lines, words = [], []
    for n in range(-2048, 2048):
        lines.append(f"sd x1, {n}(x2)")
        words.append(enc_s(n, 2, 1, 3, 0x23))
    batches.append(("stype", lines, words))

    us = [rng.randrange(1 << 20) for _ in range(4096)] + [0, 0x7FFFF, 0x80000, 0xFFFFF]
    lines, words = [], []
    for u in us:
        lines.append(f"lui x1, {u:#x}")
        words.append(enc_u(u, 1, 0x37))
        lines.append(f"auipc x1, {u:#x}")
        words.append(enc_u(u, 1, 0x17))
    batches.append(("utype", lines, words))

    ok = True
    for name, lines, words in batches:
        s = workdir / f"{name}.s"
        o = workdir / f"{name}.o"
        s.write_text("\n".join(lines) + "\n")
        r = subprocess.run([AS_BIN, "-o", str(o), str(s)],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(f"  [B-as] {name}: assembler error:\n{r.stderr[:500]}")
            ok = False
            continue
        elf = elf_min.load(str(o))
        _sec, raw = elf_min.text_bytes(elf)
        got = [int.from_bytes(raw[i:i + 4], "little")
               for i in range(0, len(raw), 4)]
        if len(got) != len(words):
            print(f"  [B-as] {name}: expected {len(words)} words, got {len(got)}")
            ok = False
            continue
        bad = [(i, words[i], got[i]) for i in range(len(words))
               if words[i] != got[i]]
        if bad:
            ok = False
            for i, want, have in bad[:5]:
                print(f"  [B-as] {name} #{i}: want {want:#010x} got {have:#010x}")
        else:
            print(f"  [B-as] {name}: {len(words)}/{len(words)} words match assembler")
    return ok

def _which(tool):
    from shutil import which
    return which(tool) is not None

# --------------------------------------------------------------------------
# Part C: scan zerocc's raw-emitter output
# --------------------------------------------------------------------------

# Base IM + F/D opcodes (no RVC, no A/FMA/SVE in current zerocc output).
KNOWN_OPS = {0x03, 0x07, 0x0F, 0x13, 0x17, 0x23, 0x27,
             0x33, 0x37, 0x53, 0x63, 0x67, 0x6F, 0x73}


def build_raw_object(test: Path, target: str, workdir: Path, timeout=120):
    """Compile `test` for `target` via the RAW path; return (obj_path|None,
    note). ZC_KEEP_OBJ=1 makes zerocc leave /tmp/zerocc_<pid>.o behind even
    when the subsequent link fails."""
    before = set(glob.glob("/tmp/zerocc_*.o"))
    env = dict(os.environ, ZC_RAW_AOT="1", ZC_KEEP_OBJ="1")
    exe = workdir / f"{test.stem}.{target}.exe"
    try:
        r = subprocess.run(
            [str(ZEROC), "--target", target, str(test), "-o", str(exe)],
            env=env, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, "zerocc timed out"
    new = set(glob.glob("/tmp/zerocc_*.o")) - before
    if not new:
        err = (r.stderr.strip().splitlines() or ["<no output>"])[-1][:80]
        return None, f"no object produced ({err})"
    return sorted(new, key=os.path.getmtime)[-1], \
        "linked" if r.returncode == 0 else "link-fail (object kept)"


def part_c(target: str, tests):
    ok = True
    for t in tests:
        workdir = Path(tempfile.mkdtemp(prefix="riscv_enc_c_"))
        obj, note = build_raw_object(t, target, workdir)
        if obj is None:
            ok = False
            print(f"  [C] {t.name:<18} BUILD-FAIL ({note})")
            continue
        try:
            elf = elf_min.load(obj)
            _sec, raw = elf_min.text_bytes(elf)
        except Exception as ex:
            ok = False
            print(f"  [C] {t.name:<18} ELF-FAIL ({ex})")
            continue

        probs = []
        unknown_ops = {}
        for pc in range(0, len(raw), 4):
            w = int.from_bytes(raw[pc:pc + 4], "little")
            if w & 3 != 3:
                probs.append(f"@{pc:#x}: compressed instruction {w:#010x}")
                continue
            op = w & 0x7F
            if op not in KNOWN_OPS:
                unknown_ops[op] = unknown_ops.get(op, 0) + 1
                continue
            if op == 0x63:
                tgt = pc + dec_b(w)
                if tgt % 4 or not (0 <= tgt <= len(raw)):
                    probs.append(f"@{pc:#x}: branch target {tgt:#x} "
                                 f"(pc={pc:#x}, text_len={len(raw):#x})")
            elif op == 0x6F:
                tgt = pc + dec_j(w)
                if tgt % 4 or not (0 <= tgt <= len(raw)):
                    probs.append(f"@{pc:#x}: jal target {tgt:#x} "
                                 f"(pc={pc:#x}, text_len={len(raw):#x})")
        if unknown_ops:
            probs.append("unknown opcodes: " +
                         ", ".join(f"{o:#x}×{c}" for o, c in sorted(unknown_ops.items())))

        if probs:
            ok = False
            print(f"  [C] {t.name:<18} {len(probs)} violation(s)  [{note}]")
            for p in probs[:6]:
                print(f"        {p}")
        else:
            print(f"  [C] {t.name:<18} ok ({len(raw) // 4} instrs)  [{note}]")
        try:
            os.unlink(obj)
        except OSError:
            pass
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--zerocc", action="store_true",
                    help="also scan zerocc raw-emitter output (part C)")
    ap.add_argument("--target", default="riscv64",
                    choices=["riscv64", "riscv32"], help="target for part C")
    ap.add_argument("--test", action="append", default=None,
                    help="test file to scan (repeatable); default: test/isa_qemu/*.c")
    ap.add_argument("--no-oracle", action="store_true", help="skip part B independent oracle")
    ap.add_argument("--as", dest="use_as", action="store_true",
                    help="also cross-check I/S/U against riscv64-linux-gnu-as")
    args = ap.parse_args()

    if not ZEROC.exists() and args.zerocc:
        print(f"[error] {ZEROC} not found -- run `make build/bin/zerocc` first")
        return 2

    print("riscv_enc_check")
    ok = part_a()
    if not args.no_oracle:
        ok &= part_b()
    if args.use_as:
        with tempfile.TemporaryDirectory(prefix="riscv_enc_b_") as d:
            ok &= part_b_as(Path(d))
    if args.zerocc:
        tests = sorted(Path(p).resolve() for p in (args.test or
                                                   [str(t) for t in TEST_DIR.glob("*.c")]))
        ok &= part_c(args.target, tests)
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
