#!/usr/bin/env python3
"""
RISC-V Backend Helper Tool

Provides:
1. IR condition code → RISC-V condition code mapping
2. Instruction sequence verification
3. Test case generation for RISC-V backend

Usage:
    python3 tools/riscv_backend_helper.py --help
"""

import argparse
import subprocess
import sys
from pathlib import Path
from dataclasses import dataclass
from typing import Dict, List, Optional

# IR condition codes
class IRCondCode:
    CC_LT = 0   # signed less than
    CC_LE = 1   # signed less or equal
    CC_GT = 2   # signed greater than
    CC_GE = 3   # signed greater or equal
    CC_EQ = 4   # equal
    CC_NE = 5   # not equal

# RISC-V condition codes for Bcc instructions
class RVCondCode:
    BEQ = "beq"   # branch if equal
    BNE = "bne"   # branch if not equal
    BLT = "blt"   # branch if less than (signed)
    BGE = "bge"   # branch if greater or equal (signed)
    BLTU = "bltu" # branch if less than (unsigned)
    BGEU = "bgeu" # branch if greater or equal (unsigned)

# IR condition code → RISC-V branch instruction mapping
IR_TO_RV_BRANCH: Dict[int, str] = {
    IRCondCode.CC_LT:  RVCondCode.BLT,
    IRCondCode.CC_LE:  RVCondCode.BGE,  # invert: !GE = LT
    IRCondCode.CC_GT:  RVCondCode.BGE,  # invert: !LE = GT (need to swap operands)
    IRCondCode.CC_GE:  RVCondCode.BLT,  # invert: !LT = GE (need to swap operands)
    IRCondCode.CC_EQ:  RVCondCode.BEQ,
    IRCondCode.CC_NE:  RVCondCode.BNE,
}

# RISC-V SLT instruction for set_cond
# For each IR condition, we need to determine:
# 1. Which SLT variant (slt/sltu)
# 2. Whether to invert result
# 3. Whether to swap operands

@dataclass
class RVSetCond:
    """RISC-V set condition instruction sequence."""
    primary_op: str      # slt or sltu
    invert_result: bool  # whether to use xnor/not
    swap_operands: bool  # whether to swap source operands
    description: str

# IR condition code → RISC-V set condition mapping
IR_TO_RV_SETCOND: Dict[int, RVSetCond] = {
    IRCondCode.CC_LT:  RVSetCond("slt", False, False, "slt rd, rs1, rs2"),
    IRCondCode.CC_LE:  RVSetCond("slt", True, False, "xnor rd, zero, (slt rs2, rs1)"),  # !LT = LE
    IRCondCode.CC_GT:  RVSetCond("slt", True, True, "xnor rd, zero, (slt rs2, rs1)"),   # !LE with swap = GT
    IRCondCode.CC_GE:  RVSetCond("slt", False, True, "slt rd, rs2, rs1"),               # swap = GE
    IRCondCode.CC_EQ:  RVSetCond("slt", False, False, "slt rd, rs1, rs2; xnor rd, zero, rd"),  # eq = !(ne)
    IRCondCode.CC_NE:  RVSetCond("slt", False, False, "slt rd, rs1, rs2; xnor rd, zero, rd"),  # ne = !(eq)
}

def print_condition_code_mapping():
    """Print IR → RISC-V condition code mapping."""
    print("\n" + "="*70)
    print("RISC-V Condition Code Mapping")
    print("="*70)
    
    print("\nIR Condition Code → RISC-V Branch Instruction:")
    print("-" * 50)
    ir_names = ["CC_LT", "CC_LE", "CC_GT", "CC_GE", "CC_EQ", "CC_NE"]
    for i, name in enumerate(ir_names):
        rv_branch = IR_TO_RV_BRANCH.get(i, "unknown")
        print(f"  {name:8} → {rv_branch}")
    
    print("\nIR Condition Code → RISC-V Set Condition:")
    print("-" * 50)
    for i, name in enumerate(ir_names):
        setcond = IR_TO_RV_SETCOND.get(i)
        if setcond:
            print(f"  {name:8} → {setcond.description}")
    
    print("\n" + "="*70)

def compile_and_analyze(source: str, isa: str, verbose: bool = False):
    """Compile C source and analyze RISC-V output."""
    root = Path.cwd()
    build_dir = root / "build" / "tmp_riscv"
    build_dir.mkdir(exist_ok=True)
    
    source_file = build_dir / "test_riscv.c"
    obj_file = build_dir / "test_riscv.o"
    
    # Write source
    source_file.write_text(source)
    
    # Compile
    zerocc = root / "build" / "bin" / "zerocc"
    cmd = [str(zerocc), "-c", str(source_file), "-o", str(obj_file), "--target", isa]
    
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        if result.returncode != 0:
            print(f"Compilation failed:\n{result.stderr}")
            return None
        
        # Check ELF header
        readelf_cmd = ["readelf", "-h", str(obj_file)]
        readelf_result = subprocess.run(readelf_cmd, capture_output=True, text=True)
        
        if "RISC-V" in readelf_result.stdout:
            if verbose:
                print(f"✓ Valid RISC-V object file generated")
                print(f"  {readelf_result.stdout.strip()}")
        else:
            print(f"⚠ Not a RISC-V object file")
            return None
        
        # Try to disassemble (may not be available)
        try:
            disasm_cmd = ["objdump", "-d", str(obj_file)]
            disasm_result = subprocess.run(disasm_cmd, capture_output=True, text=True)
            
            if "can't disassemble" in disasm_result.stderr or "UNKNOWN" in disasm_result.stdout:
                if verbose:
                    print(f"⚠ objdump doesn't support RISC-V disassembly on this system")
                return "RISC-V"
            
            if verbose:
                print(f"\nDisassembly ({isa}):")
                print("-" * 50)
                print(disasm_result.stdout)
            
            return disasm_result.stdout
        except Exception as e:
            if verbose:
                print(f"⚠ Disassembly not available: {e}")
            return "RISC-V"
        
    except Exception as e:
        print(f"Error: {e}")
        return None

def generate_test_cases():
    """Generate test cases for RISC-V backend verification."""
    tests = [
        {
            "name": "comparison_lt",
            "code": """
int test_lt(int a, int b) {
    return a < b ? 1 : 0;
}
int main() {
    return test_lt(5, 10);  // should return 1
}
""",
            "expected": 1
        },
        {
            "name": "comparison_le",
            "code": """
int test_le(int a, int b) {
    return a <= b ? 1 : 0;
}
int main() {
    return test_le(5, 5);  // should return 1
}
""",
            "expected": 1
        },
        {
            "name": "comparison_gt",
            "code": """
int test_gt(int a, int b) {
    return a > b ? 1 : 0;
}
int main() {
    return test_gt(10, 5);  // should return 1
}
""",
            "expected": 1
        },
        {
            "name": "comparison_ge",
            "code": """
int test_ge(int a, int b) {
    return a >= b ? 1 : 0;
}
int main() {
    return test_ge(5, 5);  // should return 1
}
""",
            "expected": 1
        },
        {
            "name": "comparison_eq",
            "code": """
int test_eq(int a, int b) {
    return a == b ? 1 : 0;
}
int main() {
    return test_eq(5, 5);  // should return 1
}
""",
            "expected": 1
        },
        {
            "name": "comparison_ne",
            "code": """
int test_ne(int a, int b) {
    return a != b ? 1 : 0;
}
int main() {
    return test_ne(5, 10);  // should return 1
}
""",
            "expected": 1
        },
    ]
    return tests

def main():
    parser = argparse.ArgumentParser(description="RISC-V Backend Helper Tool")
    parser.add_argument("--mapping", action="store_true", help="Print condition code mapping")
    parser.add_argument("--test", action="store_true", help="Run test cases")
    parser.add_argument("--verbose", "-v", action="store_true", help="Verbose output")
    parser.add_argument("--isa", default="riscv64", help="Target ISA (default: riscv64)")
    
    args = parser.parse_args()
    
    if args.mapping:
        print_condition_code_mapping()
        return 0
    
    if args.test:
        print("\n" + "="*70)
        print("RISC-V Backend Test Cases")
        print("="*70)
        
        tests = generate_test_cases()
        for test in tests:
            print(f"\nTest: {test['name']}")
            print("-" * 50)
            disasm = compile_and_analyze(test["code"], args.isa, verbose=args.verbose)
            if disasm:
                print(f"✓ Compiled successfully")
                # Check for expected instructions
                if "slt" in disasm or "beq" in disasm or "bne" in disasm:
                    print(f"✓ Found RISC-V conditional instructions")
                else:
                    print(f"⚠ No conditional instructions found")
            else:
                print(f"✗ Compilation failed")
        
        print("\n" + "="*70)
        return 0
    
    # Default: show help
    parser.print_help()
    return 0

if __name__ == "__main__":
    sys.exit(main())
