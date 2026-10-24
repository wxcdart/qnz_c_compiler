#!/usr/bin/env python3
"""Prologue and Epilogue Codegen Verifier.

Verifies:
  1. Selective callee-saved register (CSR) saving/restoring:
     Leaf functions without register spills/CSR pressure must NOT emit unnecessary
     CSR saves or restores (x86-64 r12-r15, aarch64 x19-x28, riscv s1-s11).
  2. Fallthrough jump elimination:
     Unconditional jumps immediately preceding their target label (e.g. `b .L_0`
     immediately before `.L_0:`) must be eliminated.
  3. Callee-save symmetry:
     Every CSR saved in a function's prologue must have a corresponding restore in
     the epilogue at the identical stack offset.

Usage:
  python3 tools/prologue_epilogue_verifier.py [--target <isa>] [--verbose]
"""

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
ZEROCC = REPO_ROOT / "build" / "bin" / "zerocc"

CSR_REGS = {
    "x86-64": [r"%rbx", r"%r12", r"%r13", r"%r14", r"%r15"],
    "aarch64": [f"x{i}" for i in range(19, 29)],
    "riscv64": [f"s{i}" for i in range(1, 12)],
    "riscv32": [f"s{i}" for i in range(1, 12)],
}

SAMPLE_C = """
int pure_leaf(void) {
    return 42;
}

int leaf_add(int a, int b) {
    return a + b;
}

int leaf_arith(int a, int b, int c, int d) {
    int x = (a + b) * (c - d);
    return x;
}

int early_ret(int x) {
    if (x <= 0) return -1;
    return x * 2;
}
"""


def compile_to_asm(c_code: str, target: str) -> str:
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as f:
        f.write(c_code)
        c_path = Path(f.name)

    asm_path = c_path.with_suffix(".s")
    try:
        cmd = [str(ZEROCC), "--target", target, "-S", str(c_path), "-o", str(asm_path)]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if res.returncode != 0:
            raise RuntimeError(f"zerocc compilation failed for {target}:\n{res.stderr}")
        return asm_path.read_text()
    finally:
        if c_path.exists():
            c_path.unlink()
        if asm_path.exists():
            asm_path.unlink()


def check_leaf_csr_omission(asm: str, target: str, func_name: str, verbose: bool) -> bool:
    """Ensure a simple leaf function emits 0 unnecessary callee-saved spills."""
    lines = asm.splitlines()
    in_func = False
    func_lines = []

    for line in lines:
        stripped = line.strip()
        if stripped == f"{func_name}:":
            in_func = True
            continue
        elif in_func and (re.match(r"^\.globl\s+", stripped) or re.match(r"^[a-zA-Z0-9_]+:\s*$", stripped)):
            # Next symbol
            break
        elif in_func:
            func_lines.append(stripped)

    func_asm = "\n".join(func_lines)
    if verbose:
        print(f"[{target}] Extracted {func_name}:\n{func_asm}\n")

    unneeded_spills = []
    if target == "aarch64":
        for reg in CSR_REGS["aarch64"]:
            if re.search(rf"\b(str|ldr)\s+{reg}\b", func_asm):
                unneeded_spills.append(reg)
    elif target in ("riscv64", "riscv32"):
        op_store = "sw" if target == "riscv32" else "sd"
        op_load = "lw" if target == "riscv32" else "ld"
        for reg in CSR_REGS[target]:
            if re.search(rf"\b({op_store}|{op_load})\s+{reg}\b", func_asm):
                unneeded_spills.append(reg)
    elif target == "x86-64":
        for reg in CSR_REGS["x86-64"]:
            # Only flag if callee-saved regs beyond rbx were blindly emitted
            if reg in (r"%r12", r"%r13", r"%r14", r"%r15"):
                if re.search(rf"\bmovq\b.*{reg}", func_asm):
                    unneeded_spills.append(reg)

    if unneeded_spills:
        print(f"FAILED [{target}:{func_name}]: Unnecessary CSRs spilled: {unneeded_spills}")
        return False

    return True


def check_fallthrough_jump_elimination(asm: str, target: str, verbose: bool) -> bool:
    """Ensure no unconditional jump immediately precedes its target label."""
    lines = [line.strip() for line in asm.splitlines() if line.strip()]
    failed = False

    for i in range(len(lines) - 1):
        cur = lines[i]
        nxt = lines[i + 1]

        # Match jmp/b/j to label
        m = re.match(r"^(?:jmp|b|j)\s+(\.[a-zA-Z0-9_]+)$", cur)
        if m:
            target_lbl = m.group(1)
            if nxt == f"{target_lbl}:":
                print(f"FAILED [{target}]: Redundant fallthrough jump found: '{cur}' followed by '{nxt}'")
                failed = True

    return not failed


def check_leaf_frameless(asm: str, target: str, func_name: str, verbose: bool) -> bool:
    """Ensure leaf function without stack variables has no frame setup."""
    lines = asm.splitlines()
    in_func = False
    func_lines = []
    for line in lines:
        stripped = line.strip()
        if stripped == f"{func_name}:":
            in_func = True
            continue
        elif in_func and (re.match(r"^\.globl\s+", stripped) or re.match(r"^[a-zA-Z0-9_]+:\s*$", stripped)):
            break
        elif in_func:
            func_lines.append(stripped)

    func_asm = "\n".join(func_lines)
    if verbose:
        print(f"[{target}:{func_name}]:\n{func_asm}\n")

    if target == "x86-64":
        if "pushq\t%rbp" in func_asm or "leave" in func_asm:
            print(f"FAILED [{target}:{func_name}]: Frame setup present in leaf function")
            return False
    elif target == "aarch64":
        if "stp\tx29, x30" in func_asm or "ldp\tx29, x30" in func_asm:
            print(f"FAILED [{target}:{func_name}]: Frame setup present in leaf function")
            return False
    elif target in ("riscv64", "riscv32"):
        if re.search(r"\bs[wd]\s+ra\b", func_asm) or re.search(r"\bs[wd]\s+s0\b", func_asm):
            print(f"FAILED [{target}:{func_name}]: Frame setup present in leaf function")
            return False

    return True


def check_x86_red_zone(asm: str, verbose: bool) -> bool:
    """Ensure leaf functions with <= 128 bytes omit subq %rsp on x86-64."""
    lines = asm.splitlines()
    in_func = False
    func_lines = []
    for line in lines:
        stripped = line.strip()
        if stripped == "leaf_add:":
            in_func = True
            continue
        elif in_func and (re.match(r"^\.globl\s+", stripped) or re.match(r"^[a-zA-Z0-9_]+:\s*$", stripped)):
            break
        elif in_func:
            func_lines.append(stripped)

    func_asm = "\n".join(func_lines)
    if "subq" in func_asm and "%rsp" in func_asm:
        print(f"FAILED [x86-64:leaf_add]: Found subq %rsp in leaf function (Red Zone missed)")
        return False
    return True


def check_symmetry(asm: str, target: str, verbose: bool) -> bool:
    """Ensure all saved CSRs are symmetrically restored."""
    lines = [line.strip() for line in asm.splitlines()]
    stores = []
    loads = []

    if target == "aarch64":
        for l in lines:
            m_str = re.search(r"str\s+(x\d+),\s*\[x29,\s*#-(\d+)\]", l)
            if m_str and m_str.group(1) in CSR_REGS["aarch64"]:
                stores.append((m_str.group(1), int(m_str.group(2))))
            m_ldr = re.search(r"ldr\s+(x\d+),\s*\[x29,\s*#-(\d+)\]", l)
            if m_ldr and m_ldr.group(1) in CSR_REGS["aarch64"]:
                loads.append((m_ldr.group(1), int(m_ldr.group(2))))
    elif target == "riscv64":
        for l in lines:
            m_str = re.search(r"sd\s+(s\d+),\s*-(\d+)\(s0\)", l)
            if m_str and m_str.group(1) in CSR_REGS["riscv64"]:
                stores.append((m_str.group(1), int(m_str.group(2))))
            m_ldr = re.search(r"ld\s+(s\d+),\s*-(\d+)\(s0\)", l)
            if m_ldr and m_ldr.group(1) in CSR_REGS["riscv64"]:
                loads.append((m_ldr.group(1), int(m_ldr.group(2))))

    if sorted(stores) != sorted(loads):
        print(f"FAILED [{target}]: Asymmetrical CSR stores ({stores}) vs loads ({loads})")
        return False
    return True


def run_checks(targets, verbose=False) -> bool:
    all_ok = True
    for target in targets:
        if verbose:
            print(f"=== Checking target: {target} ===")
        asm = compile_to_asm(SAMPLE_C, target)

        if not check_leaf_frameless(asm, target, "pure_leaf", verbose):
            all_ok = False
        if not check_leaf_frameless(asm, target, "leaf_add", verbose):
            all_ok = False
        if not check_leaf_csr_omission(asm, target, "leaf_add", verbose):
            all_ok = False
        if not check_leaf_csr_omission(asm, target, "leaf_arith", verbose):
            all_ok = False
        if target == "x86-64" and not check_x86_red_zone(asm, verbose):
            all_ok = False
        if not check_fallthrough_jump_elimination(asm, target, verbose):
            all_ok = False
        if not check_symmetry(asm, target, verbose):
            all_ok = False

    return all_ok


def main():
    parser = argparse.ArgumentParser(description="Prologue/Epilogue Codegen Verifier")
    parser.add_argument("--target", choices=["x86-64", "aarch64", "riscv64", "riscv32", "all"], default="all")
    parser.add_argument("-v", "--verbose", action="store_true", help="Verbose output")
    args = parser.parse_args()

    if not ZEROCC.exists():
        print(f"Error: {ZEROCC} not found. Run make all first.")
        sys.exit(1)

    targets = ["x86-64", "aarch64", "riscv64", "riscv32"] if args.target == "all" else [args.target]

    ok = run_checks(targets, verbose=args.verbose)
    if ok:
        print(f"Prologue/Epilogue Verifier: ALL CHECKS PASSED for targets: {', '.join(targets)}")
        sys.exit(0)
    else:
        print("Prologue/Epilogue Verifier: SOME CHECKS FAILED.")
        sys.exit(1)


if __name__ == "__main__":
    main()
