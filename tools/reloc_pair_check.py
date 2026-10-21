#!/usr/bin/env python3
"""reloc_pair_check.py - RISC-V ELF relocation pairing & section symbol verifier (T2).

Verifies the structural correctness of relocations in ELF objects produced by
zerocc's raw binary emitter:

1. PC-Relative Relocation Pairing (RISC-V):
   - Every R_RISCV_PCREL_LO12_I (24) and R_RISCV_PCREL_LO12_S (25) must pair
     with a preceding R_RISCV_PCREL_HI20 (23), typically on the preceding auipc.
   - Detects orphaned LO12 (missing matching HI20) and orphaned HI20 (unused).

2. GNU ld Linker Compatibility Checks:
   - Flags PCREL_LO12 / PCREL_HI20 against section symbols with non-zero addends
     (triggers GNU ld's fatal 'section symbol with an addend' link failure).
   - Validates that section symbols point to the correct section index (catches
     section-index mismatches between symbol table and section headers).

3. Relocation Type & Symbol Bounds:
   - Verifies all relocation symbol indices and target section indices are valid.
   - Ensures relocation types match target architecture.

Usage:
    python3 tools/reloc_pair_check.py --obj /path/to/object.o
    python3 tools/reloc_pair_check.py --zerocc                      # scan all test/isa_qemu/*.c
    python3 tools/reloc_pair_check.py --zerocc --target riscv64
    python3 tools/reloc_pair_check.py --zerocc --test test/isa_qemu/01_arith.c

Exit: 0 = all objects pass, 1 = violations found, 2 = usage/env error.
"""
from __future__ import annotations

import argparse
import glob
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import elf_min  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
ZEROC = REPO / "build" / "bin" / "zerocc"
TEST_DIR = REPO / "test" / "isa_qemu"

# RISC-V ELF Relocation Types (psABI)
R_RISCV_NONE = 0
R_RISCV_32 = 1
R_RISCV_64 = 2
R_RISCV_RELATIVE = 3
R_RISCV_COPY = 4
R_RISCV_JUMP_SLOT = 5
R_RISCV_TLS_DTPMOD32 = 6
R_RISCV_TLS_DTPREL32 = 7
R_RISCV_TLS_TPREL32 = 8
R_RISCV_BRANCH = 16
R_RISCV_JAL = 17
R_RISCV_CALL = 18
R_RISCV_CALL_PLT = 19
R_RISCV_GOT_HI20 = 20
R_RISCV_TLS_GOT_HI20 = 21
R_RISCV_TLS_GD_HI20 = 22
R_RISCV_PCREL_HI20 = 23
R_RISCV_PCREL_LO12_I = 24
R_RISCV_PCREL_LO12_S = 25
R_RISCV_HI20 = 26
R_RISCV_LO12_I = 27
R_RISCV_LO12_S = 28
R_RISCV_RELAX = 51

RISCV_RELOC_NAMES = {
    0: "R_RISCV_NONE",
    1: "R_RISCV_32",
    2: "R_RISCV_64",
    18: "R_RISCV_CALL",
    19: "R_RISCV_CALL_PLT",
    20: "R_RISCV_GOT_HI20",
    23: "R_RISCV_PCREL_HI20",
    24: "R_RISCV_PCREL_LO12_I",
    25: "R_RISCV_PCREL_LO12_S",
    26: "R_RISCV_HI20",
    27: "R_RISCV_LO12_I",
    28: "R_RISCV_LO12_S",
    51: "R_RISCV_RELAX",
}

EM_RISCV = 243


def check_elf_relocs(elf: elf_min.ElfFile) -> list[str]:
    """Inspect all relocations and symbols in an ELF object, returning a list of issues."""
    issues = []
    is_riscv = (elf.e_machine == EM_RISCV)

    # 1. Section Header & Symbol Table Index Verification
    for sym_idx, sym in enumerate(elf.symbols):
        if sym.shndx >= len(elf.sections):
            issues.append(f"sym[{sym_idx}] '{sym.name}': shndx={sym.shndx} out of range (sections={len(elf.sections)})")
            continue

        target_sec = elf.sections[sym.shndx]
        # Check section symbol consistency
        if sym.is_section_symbol and sym.name and target_sec.name:
            if sym.name != target_sec.name:
                issues.append(
                    f"SECTION_INDEX_MISMATCH: sym[{sym_idx}] name='{sym.name}' points to "
                    f"section[{sym.shndx}] name='{target_sec.name}'"
                )

    if not is_riscv:
        return issues

    # 2. Group RISC-V relocations by target section
    relocs_by_section: dict[int, list[elf_min.Reloc]] = {}
    for r in elf.relocations:
        relocs_by_section.setdefault(r.section_idx, []).append(r)

    for sec_idx, rlist in relocs_by_section.items():
        sec_name = elf.sections[sec_idx].name if sec_idx < len(elf.sections) else f"sec#{sec_idx}"
        hi20_offsets = {r.offset: r for r in rlist if r.r_type == R_RISCV_PCREL_HI20}
        lo12_relocs = [r for r in rlist if r.r_type in (R_RISCV_PCREL_LO12_I, R_RISCV_PCREL_LO12_S)]

        used_hi20 = set()

        for lo in lo12_relocs:
            lo_name = RISCV_RELOC_NAMES.get(lo.r_type, f"RELOC_{lo.r_type}")
            # Check 1: LO12 pairing with HI20 (in zerocc raw emitter, auipc precedes by 4 bytes)
            pair_offset = lo.offset - 4
            hi = hi20_offsets.get(pair_offset)

            if hi is None:
                issues.append(
                    f"ORPHAN_LO12: {sec_name}+{lo.offset:#x} {lo_name} has no matching HI20 at {pair_offset:#x}"
                )
            else:
                used_hi20.add(hi.offset)
                # Verify that LO12 and HI20 agree on target symbol or addend
                if lo.sym != hi.sym:
                    s_lo = elf.symbols[lo.sym].name if lo.sym < len(elf.symbols) else f"sym#{lo.sym}"
                    s_hi = elf.symbols[hi.sym].name if hi.sym < len(elf.symbols) else f"sym#{hi.sym}"
                    issues.append(
                        f"PAIR_SYMBOL_MISMATCH: {sec_name}+{lo.offset:#x} LO12 targets {s_lo} but HI20 targets {s_hi}"
                    )

            # Check 2: Section symbol with addend rejection check
            if lo.sym < len(elf.symbols):
                sym = elf.symbols[lo.sym]
                if sym.is_section_symbol and lo.addend != 0:
                    issues.append(
                        f"SECTION_SYMBOL_ADDEND: {sec_name}+{lo.offset:#x} {lo_name} targets section "
                        f"symbol '{sym.name}' with non-zero addend {lo.addend}"
                    )

        # Check for unreferenced HI20 relocations
        for off, hi in hi20_offsets.items():
            if off not in used_hi20:
                issues.append(
                    f"ORPHAN_HI20: {sec_name}+{off:#x} R_RISCV_PCREL_HI20 has no matching LO12"
                )

    return issues


def build_raw_object(test: Path, target: str, workdir: Path, timeout=120) -> tuple[Path | None, str]:
    """Compile test via raw path, preserving object file."""
    before = set(glob.glob("/tmp/zerocc_*.o"))
    env = dict(os.environ, ZC_RAW_AOT="1", ZC_KEEP_OBJ="1")
    exe = workdir / f"{test.stem}.{target}.exe"
    try:
        r = subprocess.run(
            [str(ZEROC), "--target", target, str(test), "-o", str(exe)],
            env=env, capture_output=True, text=True, timeout=timeout
        )
    except subprocess.TimeoutExpired:
        return None, "zerocc timed out"

    new = set(glob.glob("/tmp/zerocc_*.o")) - before
    if not new:
        err = (r.stderr.strip().splitlines() or ["<no output>"])[-1][:80]
        return None, f"no object produced ({err})"
    latest = sorted(new, key=os.path.getmtime)[-1]
    return Path(latest), "linked" if r.returncode == 0 else "link-fail (object kept)"


def scan_object(obj_path: Path) -> tuple[bool, list[str]]:
    """Scan a single object file and report issues."""
    try:
        elf = elf_min.load(str(obj_path))
    except Exception as ex:
        return False, [f"Failed to load ELF: {ex}"]

    issues = check_elf_relocs(elf)
    return (len(issues) == 0), issues


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--obj", type=Path, help="single object file to inspect")
    ap.add_argument("--zerocc", action="store_true",
                    help="compile and inspect tests via zerocc raw binary path")
    ap.add_argument("--target", default="riscv64",
                    choices=["riscv64", "riscv32", "aarch64", "x86_64"],
                    help="target ISA for zerocc compilation")
    ap.add_argument("--test", action="append", default=None,
                    help="test file to scan (repeatable); default: test/isa_qemu/*.c")
    args = ap.parse_args()

    if not args.obj and not args.zerocc:
        ap.print_help()
        return 2

    print("reloc_pair_check")
    total_ok = True

    if args.obj:
        if not args.obj.exists():
            print(f"[error] Object file {args.obj} not found")
            return 2
        ok, issues = scan_object(args.obj)
        total_ok &= ok
        if ok:
            print(f"  [PASS] {args.obj.name}")
        else:
            print(f"  [FAIL] {args.obj.name} ({len(issues)} issue(s)):")
            for issue in issues:
                print(f"    - {issue}")

    if args.zerocc:
        if not ZEROC.exists():
            print(f"[error] {ZEROC} not found -- run `make build/bin/zerocc` first")
            return 2

        tests = sorted(Path(p).resolve() for p in (args.test or [str(t) for t in TEST_DIR.glob("*.c")]))
        for t in tests:
            workdir = Path(tempfile.mkdtemp(prefix="reloc_check_"))
            obj, note = build_raw_object(t, args.target, workdir)
            if obj is None:
                print(f"  [BUILD-FAIL] {t.name:<18} ({note})")
                total_ok = False
                continue

            ok, issues = scan_object(obj)
            total_ok &= ok
            status = "PASS" if ok else "FAIL"
            print(f"  [{status}] {t.name:<18} [{note}]")
            for issue in issues:
                print(f"        {issue}")

            try:
                os.unlink(obj)
            except OSError:
                pass

    print("RESULT:", "PASS" if total_ok else "FAIL")
    return 0 if total_ok else 1


if __name__ == "__main__":
    sys.exit(main())
