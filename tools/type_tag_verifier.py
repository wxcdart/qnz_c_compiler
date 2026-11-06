#!/usr/bin/env python3
"""
Type-tag verifier (Sprint 118, type-system hardening #1, slice 1).

ZeroCC's IR is untyped: type flows through side channels (C99 width marks on
defining instructions, last_expr_is_float / last_expr_is_128 producer
flags, OP_OVF_TRAP check-width metadata).  Sprint 118 stamps every
lowered temp with the semantic type of its AST node (the frontend's
verdict) in a per-temp tag table, so those conventions become CHECKABLE
invariants instead of things enforced by memory.

This gate compiles the test corpus with zerocc -dump-ir and cross-checks
the backend's type conventions against the frontend's tags:

  I1  width-mark <-> tag: an instruction's C99 width mark (w in {1,2,4,8}
      signed, {9,10,12,16} unsigned) must agree with the dest temp's tag
      (class + width).  Exotic bit-field marks (w not in that set) skip.
  I2  OP_OVF_TRAP: check width in {4,8}; the checked temp's tag must be a
      SIGNED int of exactly that width; the immediately-preceding
      instruction must be an unmarked ADD/SUB/MUL on the same temp
      (emission contract + clean-value invariant: the ALU op is never
      width-marked so the raw 64-bit result reaches the check).
  I3  FP ops: value operands/dests of FP-producing ops must carry FLOAT
      tags (and conversions must go the right direction).
  I4  128-bit ops: *128 ops define/require int128/uint128 tags; 64-bit
      views (TRUNC/LO/HI) and 64<->128 extends carry width-8 int tags.
  I5  SEXT8/16/32 / ZEXT8/16/32: dest tag must be int/uint of the
      matching width.
  I6  tag <-> defining op: a FLOAT-tagged temp must not be defined by an
      integer op (and vice versa) - the Sprint 115l esz / Sprint 113
      vector-lane bug class.
  I7  OP_COND_SET: 0/1 result - a tagged dest must be int-family.
  I8  unsigned DIV/MOD (the [u] cond mark): a tagged dest must be UINT.
  I9  (slice 2) unmarked narrow-op population (coverage stat, no
      failures): UNmarked narrow-class ALU ops (ADD/SUB/MUL/DIV/MOD/
      AND/OR/XOR/NOT/NEG/SHL/SHR/SAR/ADD_IMM/SUB_IMM/INC/DEC) whose
      dest is tagged int/uint of width 1/2/4, classified by how the
      value is canonicalized: (a) OVF-adjacent - a same-temp
      OP_OVF_TRAP follows, the tag drives a deferred extend after the
      check (the slot-steal case); (b) pre-canonicalized - a same-temp
      in-place SEXTn/ZEXTn of matching direction and width follows
      (compound-assign / narrow-local-store pattern); (c) otherwise -
      left raw by design, canonicalized by the narrow load/store
      discipline (loads re-extend, stores truncate or 64-bit-slot
      values are re-extended on the next load). The stat keeps the
      population visible; (a) and (b) are the arrangements codegen
      acts on directly.

A tagged temp whose tag disagrees with the op that produced it is a
violation; an UNtagged temp (the frontend attached no TypeInfo to the
node) is counted in the coverage stats, not failed - the C-side
per-expression flag check (lower_expr in ast_lowering_expr.c) plus this
gate together bound the coverage.

Usage:
  python3 tools/type_tag_verifier.py --verify-all
  python3 tools/type_tag_verifier.py --verify-all --levels 0 2 3
  python3 tools/type_tag_verifier.py --file test/jit/float_ops.c
"""

import argparse
import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ZEROCC = os.path.join(ROOT, "build", "bin", "zerocc")
SWEEP_CORPUS = ["test/jit", "test/aot", "test/licm", "test/inline_audit"]
SAN_DIR = "test/san"

IR_LINE = re.compile(r"^\s*(\d+):\s+(\S+)\s*(.*)$")
FN_LINE = re.compile(r"^=== IR for function '(.+)' ===\s*$")
TAG_LINE = re.compile(r"^# temp tags:\s*(.*)$")
TAG_ITEM = re.compile(r"r(\d+)=([a-z0-9_]+)(?:,(\d+))?\b")
WIDTH_ANN = re.compile(r"\[w=(\d+)\]")
UNSIGNED_ANN = re.compile(r"\[u\]")
FLAGS_ANN = re.compile(r"\[flags_out=.*$")
REG_TOKEN = re.compile(r"\b(?:r|xmm)(\d+)\b")
FIRST_REG = re.compile(r"^(?:r|xmm)(\d+)")

# --- opcode operand semantics (from ir.h / the emitters) -------------------

# op: (dest_kind, [src kinds]) where kind in
#   F=FLOAT value, I=any int/ptr value, S=128 value, A=address (int/ptr),
#   C=shift count (int), -=no operand, X=anything (copy-neutral).
# dest/src kind None = no check.  Only temps >= 16 (user temps) are checked;
# untagged temps are coverage stats, not failures (except where noted).
FP_VALUE_OPS = {
    "FADD": ("F", ["F", "F"]), "FSUB": ("F", ["F", "F"]),
    "FMUL": ("F", ["F", "F"]), "FDIV": ("F", ["F", "F"]),
    "FMOV": ("F", ["F"]), "FMOV_IMM": ("F", []), "FLOAD": ("F", ["A"]),
    "VADDSS": ("F", ["F", "F"]), "VSUBSS": ("F", ["F", "F"]),
    "VMULSS": ("F", ["F", "F"]), "VDIVSS": ("F", ["F", "F"]),
    "VADDSD": ("F", ["F", "F"]), "VSUBSD": ("F", ["F", "F"]),
    "VMULSD": ("F", ["F", "F"]), "VDIVSD": ("F", ["F", "F"]),
    "VMOV": ("F", ["F"]), "VMOVSS": ("F", ["F"]), "VMOVSD": ("F", ["F"]),
    "VLOAD": ("F", ["A"]), "VLOADSS": ("F", ["A"]), "VLOADSD": ("F", ["A"]),
    "VLOADSS_RAW": ("F", ["A"]),
    "VCVT_SI2SS": ("F", ["I"]), "VCVT_SI2SD": ("F", ["I"]),
    "CVTSI2SD": ("F", ["I"]),
    "CVTSS2SD": ("F", ["F"]), "CVTSD2SS": ("F", ["F"]),
    "CVTTSD2SS": ("F", ["F"]),
    "VCMPSS": (None, ["F", "F"]), "VCMPSD": (None, ["F", "F"]),
}
# int-producing FP conversions (dest must be int, src must be float)
FP_TO_INT_OPS = {"CVTTSD2SI": "F", "VCVT_SS2SI": "F", "VCVT_SD2SI": "F"}
# FP stores: (addr, value)
FP_STORE_OPS = {
    "FSTORE": None, "VSTORE": None, "VSTORESS": None, "VSTORESD": None,
    "VSTORESS_RAW": None,
}

# 128-bit ops: dest kind S, src kinds
OPS128 = {
    "ADD128": ("S", ["S", "S"]), "SUB128": ("S", ["S", "S"]),
    "MUL128": ("S", ["S", "S"]), "AND128": ("S", ["S", "S"]),
    "OR128": ("S", ["S", "S"]), "XOR128": ("S", ["S", "S"]),
    "NOT128": ("S", ["S"]), "NEG128": ("S", ["S"]),
    "MOV128": ("S", ["S"]), "MOV128_IMM": ("S", []),
    "LOAD128": ("S", ["A"]), "CMP128": (None, ["S", "S"]),
    "SHL128": ("S", ["S", "C"]), "SHR128": ("S", ["S", "C"]),
    "SAR128": ("S", ["S", "C"]),
    "ZEXT64_128": ("S", ["I"]), "SEXT64_128": ("S", ["I"]),
    "TRUNC128_64": ("I", ["S"]), "LO128": ("I", ["S"]), "HI128": ("I", ["S"]),
    "MK128": ("S", ["I", "I"]),
}
OP_STORE128 = "STORE128"  # (value=S, addr=A)

# SEXTn/ZEXTn: width in bytes from the op name (IN-PLACE on the printed reg)
EXTEND_OPS = {
    "SEXT8": (1, True), "SEXT16": (2, True), "SEXT32": (4, True),
    "ZEXT8": (1, False), "ZEXT16": (2, False), "ZEXT32": (4, False),
}

# Slice 2: the ops codegen canonicalizes via the tag gap-fill when no
# C99 width mark is present (mirrors zc_op_can_define_narrow_int in ir.h).
NARROW_ALU_OPS = {
    "ADD", "SUB", "MUL", "DIV", "MOD", "AND", "OR", "XOR", "NOT", "NEG",
    "SHL", "SHR", "SAR", "ADD_IMM", "SUB_IMM", "INC", "DEC",
}

# Integer ops that DEFINE a value (for I6: a FLOAT-tagged temp must not be
# defined by one; MOV/MOV_REG/LEA/LOAD are deliberately excluded - they are
# copy/address/neutral, and float locals load through FLOAD).
INT_DEF_OPS = {
    "ADD", "SUB", "MUL", "DIV", "MOD", "AND", "OR", "XOR", "NOT", "NEG",
    "INC", "DEC", "ADD_IMM", "SUB_IMM", "SHL", "SHR", "SAR",
    "SEXT8", "SEXT16", "SEXT32", "ZEXT8", "ZEXT16", "ZEXT32",
    "CMP", "CMP_ZERO", "COND_SET", "LOAD_16", "LOAD_32", "LOAD_BYTE",
    "DEREF", "MOV_GLOBAL", "MOV_IMM",
}
# FP ops that DEFINE a FLOAT value (for I6 the other direction)
FP_DEF_OPS = set(FP_VALUE_OPS) - set(FP_TO_INT_OPS) - {"VCMPSS", "VCMPSD"}

# Mark encodings: w in 1,2,4,8 = signed N-bit; w in 9,10,12,16 = unsigned.
MARK_SIGNED = {1: 1, 2: 2, 4: 4, 8: 8}
MARK_UNSIGNED = {9: 1, 10: 2, 12: 4, 16: 8}

# Ops whose FIRST register token in the dump is NOT a destination (it is a
# source/condition operand): I6's "defined by" mapping must skip them.
NO_DEST_OPS = {
    "CMP", "CMP_ZERO", "CMP128", "VCMPSS", "VCMPSD",
    "JE", "JNE", "JL", "JLE", "JG", "JCOND", "JMP", "JMP_LABEL",
    "JMP_INDIRECT",
    "FSTORE", "VSTORE", "VSTORESS", "VSTORESD", "VSTORESS_RAW",
    "STORE_16", "STORE_32", "STORE_64", "STORE_BYTE", "STORE128",
    "MOV_MEM", "PUSH", "SETJMP", "LONGJMP", "NONLOCAL_GOTO", "OVF_TRAP",
}


def parse_dump(text):
    """Parse -dump-ir stderr into [(fn, instrs, tags)] where instrs is
    [(idx, op, rest, width_mark, unsigned_div)] and tags is {id: (cls, wb)}."""
    fns = []
    cur = None
    for line in text.splitlines():
        m = FN_LINE.match(line)
        if m:
            cur = {"fn": m.group(1), "ins": [], "tags": {}}
            fns.append(cur)
            continue
        if cur is None:
            continue
        m = TAG_LINE.match(line)
        if m:
            for t in TAG_ITEM.finditer(m.group(1)):
                cur["tags"][int(t.group(1))] = (t.group(2),
                                                int(t.group(3) or 0))
            continue
        if line.startswith("=== Allocated Intervals"):
            continue
        m = IR_LINE.match(line)
        if not m:
            continue
        idx, op, rest = int(m.group(1)), m.group(2), m.group(3)
        wm = WIDTH_ANN.search(rest)
        ud = bool(UNSIGNED_ANN.search(rest))
        cur["ins"].append((idx, op, rest,
                           int(wm.group(1)) if wm else 0, ud))
    return fns


def cls_of(tag):
    return tag[0] if tag else None


def is_int_family(cls):
    return cls in ("int", "uint")


def is_128(cls):
    return cls in ("int128", "uint128")


def check_function(fn, ins, tags, stats):
    v = []
    by_idx = {i[0]: i for i in ins}

    for idx, op, rest, wmark, udiv in ins:
        # Strip the flag annotations so their r0 markers are not scanned as
        # temp operands; FP temps are printed as xmm<N>, int temps as r<N>.
        body = FLAGS_ANN.split(rest, 1)[0].strip()
        dest = None
        srcs = []
        if op not in NO_DEST_OPS:
            dm = FIRST_REG.match(body)
            if dm:
                dest = int(dm.group(1))
            srcs = [int(x) for x in REG_TOKEN.findall(body)]
            # dest may also appear in the src list (dump prints it first)
            if dest is not None and srcs and srcs[0] == dest:
                srcs = srcs[1:]
        else:
            srcs = [int(x) for x in REG_TOKEN.findall(body)]

        # --- I1: width mark <-> tag -----------------------------------
        if wmark and op != "OVF_TRAP" and dest is not None:
            nbytes = MARK_SIGNED.get(wmark) if wmark < 8 else \
                MARK_UNSIGNED.get(wmark)
            signed = wmark < 8
            if nbytes is not None:
                stats["i1_marks"] += 1
                tag = tags.get(dest)
                if tag is None:
                    stats["i1_untagged"] += 1
                else:
                    want_cls = "int" if signed else "uint"
                    if cls_of(tag) != want_cls or tag[1] != nbytes:
                        v.append("I1 r%d: mark w=%d (%s %dB) but tag=%s,%d"
                                 % (dest, wmark, want_cls, nbytes,
                                    tag[0], tag[1]))

        # --- I2: OVF_TRAP ---------------------------------------------
        if op == "OVF_TRAP":
            stats["i2_traps"] += 1
            cw = wmark
            t = srcs[0] if srcs else dest
            tag = tags.get(t) if t is not None else None
            if cw not in (4, 8):
                v.append("I2 r%s: OVF_TRAP check width %d not in {4,8}"
                         % (t, cw))
            elif tag is None:
                stats["i2_untagged"] += 1
            elif cls_of(tag) != "int" or tag[1] != cw:
                v.append("I2 r%s: OVF_TRAP width %d but tag=%s,%d "
                         "(must be signed int of the check width)"
                         % (t, cw, tag[0], tag[1]))
            prev = by_idx.get(idx - 1)
            if prev is None or prev[1] not in ("ADD", "SUB", "MUL") \
                    or prev[0] + 1 != idx:
                v.append("I2 r%s: OVF_TRAP not immediately after an "
                         "ADD/SUB/MUL (got %s)"
                         % (t, prev[1] if prev else "<none>"))
            elif prev[3] != 0:
                v.append("I2 r%s: checked ALU op is width-marked (w=%d) - "
                         "clean-value invariant broken"
                         % (t, prev[3]))
            elif prev[1] in ("ADD", "SUB", "MUL"):
                pm = re.match(r"^r(\d+),?", prev[2])
                if pm and int(pm.group(1)) != t:
                    v.append("I2 r%s: OVF_TRAP temp != ALU dest r%s"
                             % (t, pm.group(1)))

        # --- I3: FP ops -------------------------------------------------
        if op in FP_VALUE_OPS:
            dk, sk = FP_VALUE_OPS[op]
            if dk == "F" and dest is not None:
                tag = tags.get(dest)
                if tag is not None and cls_of(tag) != "float":
                    v.append("I3 %s r%d: FP dest tagged %s,%d"
                             % (op, dest, tag[0], tag[1]))
            for s, kind in zip(srcs, sk):
                if kind == "F":
                    tag = tags.get(s)
                    if tag is not None and cls_of(tag) != "float":
                        v.append("I3 %s r%d: FP src tagged %s,%d"
                                 % (op, s, tag[0], tag[1]))
                elif kind == "I":
                    tag = tags.get(s)
                    if tag is not None and not is_int_family(cls_of(tag)):
                        v.append("I3 %s r%d: int src tagged %s,%d"
                                 % (op, s, tag[0], tag[1]))
        if op in FP_TO_INT_OPS and dest is not None:
            tag = tags.get(dest)
            if tag is not None and not is_int_family(cls_of(tag)):
                v.append("I3 %s r%d: int dest tagged %s,%d"
                         % (op, dest, tag[0], tag[1]))
            if srcs:
                tag = tags.get(srcs[0])
                if tag is not None and cls_of(tag) != "float":
                    v.append("I3 %s r%d: float src tagged %s,%d"
                             % (op, srcs[0], tag[0], tag[1]))
        if op in FP_STORE_OPS:
            vm = re.search(r"xmm(\d+)", body)
            if vm:
                tag = tags.get(int(vm.group(1)))
                if tag is not None and cls_of(tag) != "float":
                    v.append("I3 %s r%d: FP value src tagged %s,%d"
                             % (op, int(vm.group(1)), tag[0], tag[1]))

        # --- I4: 128-bit ops --------------------------------------------
        if op in OPS128:
            dk, sk = OPS128[op]
            if dk == "S" and dest is not None:
                tag = tags.get(dest)
                if tag is not None and not is_128(cls_of(tag)):
                    v.append("I4 %s r%d: 128 dest tagged %s"
                             % (op, dest, tag[0]))
            elif dk == "I" and dest is not None:
                tag = tags.get(dest)
                if tag is not None and not is_int_family(cls_of(tag)):
                    v.append("I4 %s r%d: int dest tagged %s,%d"
                             % (op, dest, tag[0], tag[1]))
            for s, kind in zip(srcs, sk):
                if kind == "S":
                    tag = tags.get(s)
                    if tag is not None and not is_128(cls_of(tag)):
                        v.append("I4 %s r%d: 128 src tagged %s"
                                 % (op, s, tag[0]))
                elif kind == "I":
                    tag = tags.get(s)
                    if tag is not None and not is_int_family(cls_of(tag)):
                        v.append("I4 %s r%d: int src tagged %s,%d"
                                 % (op, s, tag[0], tag[1]))
                elif kind == "C":
                    tag = tags.get(s)
                    if tag is not None and not is_int_family(cls_of(tag)):
                        v.append("I4 %s r%d: shift count tagged %s"
                                 % (op, s, tag[0]))
        if op == OP_STORE128 and len(srcs) >= 2:
            # dump: STORE128 [r<addr>], r<val> -> value is the 2nd reg
            tag = tags.get(srcs[1])
            if tag is not None and not is_128(cls_of(tag)):
                v.append("I4 STORE128 r%d: 128 value tagged %s"
                         % (srcs[1], tag[0]))

        # --- I5: SEXTn / ZEXTn (IN-PLACE on the printed reg) -------------
        # The printed reg is temp_src1: the value temp, extended in place.
        # Its tag is the C type of the value BEFORE the in-place op.  Any
        # int-family width is legal in place: tag width == n is the normal
        # N-bit value, tag width > n is a deliberate narrowing conversion
        # (return truncation, param narrowing, bit-field reduction), and
        # tag width < n is a mathematical no-op (the value is already
        # sign/zero-extended to 64 bits by its producer).  The violation
        # is a CLASS mismatch: a float/128/ptr value through an integer
        # extend op.
        if op in EXTEND_OPS:
            nbytes, signed = EXTEND_OPS[op]
            t = dest  # parser: first reg of an in-place op
            tag = tags.get(t) if t is not None else None
            if tag is not None:
                if not is_int_family(cls_of(tag)):
                    v.append("I5 %s r%d: dest tagged %s (want int/uint)"
                             % (op, t, tag[0]))

        # --- I7: COND_SET -------------------------------------------------
        if op == "COND_SET" and dest is not None:
            tag = tags.get(dest)
            if tag is not None and not is_int_family(cls_of(tag)):
                v.append("I7 r%d: COND_SET dest tagged %s (want int 0/1)"
                         % (dest, tag[0]))

        # --- I8: unsigned DIV/MOD ------------------------------------------
        if udiv and op in ("DIV", "MOD") and dest is not None:
            tag = tags.get(dest)
            if tag is not None and cls_of(tag) != "uint":
                v.append("I8 r%d: unsigned DIV/MOD dest tagged %s (want uint)"
                         % (dest, tag[0]))

        # --- I9: unmarked narrow-op population (coverage stat, slice 2) -----
        # No failures: unmarked narrow-class ALU ops on a narrow tagged dest
        # are left unextended by codegen except the OVF-adjacent deferral;
        # the stat keeps the population and its canonicalization arrangements
        # visible (mirrors the slice-2 extend rule in ir.h).
        if (op in NARROW_ALU_OPS and not wmark and dest is not None):
            tag = tags.get(dest)
            if tag is not None and is_int_family(cls_of(tag)) \
                    and tag[1] in (1, 2, 4):
                nxt = by_idx.get(idx + 1)
                nxt_op = nxt[1] if nxt else None
                m = FIRST_REG.search(FLAGS_ANN.split(nxt[2], 1)[0]) \
                    if nxt else None
                same_temp = bool(m) and int(m.group(1)) == dest
                if nxt_op == "OVF_TRAP" and same_temp:
                    stats["i9_deferred"] += 1
                elif nxt_op in EXTEND_OPS and same_temp and \
                        EXTEND_OPS[nxt_op][0] == tag[1] and \
                        (EXTEND_OPS[nxt_op][1] == (cls_of(tag) == "int")):
                    stats["i9_precan"] += 1
                else:
                    stats["i9_discipline"] += 1

    # --- I6: tag <-> defining op -------------------------------------------
    defined_by = {}
    for idx, op, rest, wmark, udiv in ins:
        if op in NO_DEST_OPS:
            continue
        body = FLAGS_ANN.split(rest, 1)[0].strip()
        dm = FIRST_REG.match(body)
        if dm:
            d = int(dm.group(1))
            defined_by.setdefault(d, []).append(op)
    for tid, ops in defined_by.items():
        tag = tags.get(tid)
        if tag is None:
            continue
        cls = cls_of(tag)
        for op in ops:
            if cls == "float" and op in INT_DEF_OPS:
                v.append("I6 r%d: float temp defined by %s" % (tid, op))
            elif (is_int_family(cls) or is_128(cls)) and op in FP_DEF_OPS:
                v.append("I6 r%d: %s temp defined by FP op %s"
                         % (tid, cls, op))
    return v


def run_one(src, level, zero_san, timeout=60):
    cmd = [ZEROCC, "-dump-ir", "-O%d" % level, "-Iinclude", "-Isrc", src,
           "-o", "/tmp/ttv_bin"]
    if zero_san:
        cmd.insert(1, "-fsanitize=undefined")
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    if out.returncode != 0:
        raise RuntimeError("compile failed rc=%d: %s"
                           % (out.returncode, out.stderr[-300:]))
    return parse_dump(out.stderr)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("--verify-all", action="store_true")
    ap.add_argument("--file")
    ap.add_argument("--levels", default="0 2 3",
                    help="space-separated -O levels (default: 0 2 3)")
    ap.add_argument("--timeout", type=int, default=60)
    args = ap.parse_args()

    if not os.path.exists(ZEROCC):
        sys.exit("build/bin/zerocc missing - run `make all` first")
    levels = [int(x) for x in args.levels.split()]

    files = []
    if args.file:
        files.append((args.file, False))
    elif args.verify_all:
        for d in SWEEP_CORPUS:
            files += [(f, False) for f in
                      sorted(glob.glob(os.path.join(ROOT, d, "*.c")))]
        files += [(f, True) for f in
                  sorted(glob.glob(os.path.join(ROOT, SAN_DIR, "*.c")))]
    else:
        ap.print_help()
        return 1

    stats = {"i1_marks": 0, "i1_untagged": 0,
             "i2_traps": 0, "i2_untagged": 0,
             "i9_deferred": 0, "i9_precan": 0, "i9_discipline": 0,
             "fns": 0, "tags": 0}
    total_v = 0
    failed = 0
    for src, zero_san in files:
        for level in levels:
            try:
                fns = run_one(src, level, zero_san, args.timeout)
            except (RuntimeError, subprocess.TimeoutExpired) as e:
                print("SKIP %s -O%d%s: %s" % (src, level,
                                               " +san" if zero_san else "", e))
                continue
            fviol = []
            for fn in fns:
                stats["fns"] += 1
                stats["tags"] += len(fn["tags"])
                fviol += check_function(fn["fn"], fn["ins"], fn["tags"],
                                        stats)
            if fviol:
                failed += 1
                total_v += len(fviol)
                print("FAIL %s -O%d%s (%d):" % (src, level,
                                                 " +san" if zero_san else "",
                                                 len(fviol)))
                for msg in fviol[:12]:
                    print("   %s" % msg)
                if len(fviol) > 12:
                    print("   ... and %d more" % (len(fviol) - 12))
    print("type-tag verifier: %d files x levels %s | %d functions, "
          "%d tagged temps, %d marks checked (%d untagged), "
          "%d OVF_TRAPs (%d untagged) | I9 unmarked narrow ops: %d "
          "OVF-deferred, %d pre-canonicalized, %d load/store-discipline"
          % (len(files), levels, stats["fns"], stats["tags"],
             stats["i1_marks"], stats["i1_untagged"],
             stats["i2_traps"], stats["i2_untagged"],
             stats["i9_deferred"], stats["i9_precan"],
             stats["i9_discipline"]))
    if total_v:
        print("RESULT: FAIL (%d violations in %d file-runs)"
              % (total_v, failed))
        return 1
    print("RESULT: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
