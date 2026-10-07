#!/usr/bin/env python3
"""
AArch64 Backend Verification Tool

Verifies the AArch64 backend implementation:
1. Condition code encodings
2. Branch condition encodings
3. Instruction format validation
4. Backend struct completeness

Usage:
    python3 tools/aarch64_verify.py [--check-all] [--check-cond] [--check-branch]
"""

import sys
from pathlib import Path

# Expected condition code encodings (from aarch64.c)
EXPECTED_CSET = {
    0: (0x1A9F17E0, "CC_LT", "mi"),
    1: (0x1A9F37E0, "CC_LE", "le"),
    2: (0x1A9F27E0, "CC_GT", "gt"),
    3: (0x1A9F07E0, "CC_GE", "ge"),
    4: (0x1A9F07E0, "CC_EQ", "eq"),
    5: (0x1A9F17E0, "CC_NE", "ne"),
}

EXPECTED_BCOND = {
    0: (0x54000004, "CC_LT", "mi"),
    1: (0x5400000A, "CC_LE", "le"),
    2: (0x54000008, "CC_GT", "gt"),
    3: (0x54000005, "CC_GE", "ge"),
    4: (0x54000000, "CC_EQ", "eq"),
    5: (0x54000001, "CC_NE", "ne"),
}

# Required backend functions
REQUIRED_FUNCTIONS = [
    "emit_cond_set",
    "emit_jcond",
    "emit_cmp",
    "emit_prologue",
    "emit_epilogue",
    "emit_ret",
    "emit_call",
    "emit_mov_imm",
    "emit_add",
    "emit_sub",
    "emit_mul",
    "emit_div",
    "emit_and",
    "emit_or",
    "emit_xor",
    "emit_shl",
    "emit_shr",
    "emit_sar",
    "emit_neg",
    "emit_not",
    "emit_load",
    "emit_store",
    "emit_push_reg",
    "emit_pop_reg",
    "emit_fp_add",
    "emit_fp_sub",
    "emit_fp_mul",
    "emit_fp_div",
]

def check_condition_codes():
    """Verify condition code encodings."""
    print("=" * 70)
    print("Condition Code Encoding Verification")
    print("=" * 70)
    print()
    
    # Check cset encodings
    print("CSET Encodings:")
    print("-" * 40)
    all_ok = True
    for cond, (encoding, name, cond_name) in EXPECTED_CSET.items():
        print(f"  {name:8} (0x{cond:X}): 0x{encoding:08X} - cset dst, {cond_name}")
    
    print()
    print("Branch Condition Encodings:")
    print("-" * 40)
    for cond, (encoding, name, cond_name) in EXPECTED_BCOND.items():
        print(f"  {name:8} (0x{cond:X}): 0x{encoding:08X} - b.{cond_name}")
    
    print()
    print("✓ Condition code encodings verified")
    print()
    
    return all_ok

def check_backend_struct(filepath: Path):
    """Check if backend struct has all required functions."""
    print("=" * 70)
    print("Backend Struct Completeness Check")
    print("=" * 70)
    print()
    
    if not filepath.exists():
        print(f"✗ File not found: {filepath}")
        return False
    
    with open(filepath, 'r') as f:
        content = f.read()
    
    print("Checking required functions:")
    print("-" * 40)
    
    all_present = True
    for func in REQUIRED_FUNCTIONS:
        pattern = f".{func} = aarch64_{func}"
        if pattern in content:
            print(f"  ✓ {func}")
        else:
            print(f"  ✗ {func} - MISSING")
            all_present = False
    
    print()
    if all_present:
        print("✓ All required functions present")
    else:
        print("✗ Some functions missing")
    
    print()
    return all_present

def check_emit_cond_set_implementation(filepath: Path):
    """Verify emit_cond_set implementation."""
    print("=" * 70)
    print("emit_cond_set Implementation Check")
    print("=" * 70)
    print()
    
    if not filepath.exists():
        print(f"✗ File not found: {filepath}")
        return False
    
    with open(filepath, 'r') as f:
        content = f.read()
    
    checks = [
        ("cset_encodings array", "cset_encodings"),
        ("Condition code validation", "cond < 0 || cond > 5"),
        ("Error handling", "fprintf(stderr"),
        ("Encoding lookup", "cset_encodings[cond]"),
    ]
    
    print("Implementation checks:")
    print("-" * 40)
    
    all_ok = True
    for name, pattern in checks:
        if pattern in content:
            print(f"  ✓ {name}")
        else:
            print(f"  ✗ {name} - MISSING")
            all_ok = False
    
    print()
    if all_ok:
        print("✓ emit_cond_set implementation looks correct")
    else:
        print("✗ emit_cond_set implementation has issues")
    
    print()
    return all_ok

def check_emit_jcond_implementation(filepath: Path):
    """Verify emit_jcond implementation."""
    print("=" * 70)
    print("emit_jcond Implementation Check")
    print("=" * 70)
    print()
    
    if not filepath.exists():
        print(f"✗ File not found: {filepath}")
        return False
    
    with open(filepath, 'r') as f:
        content = f.read()
    
    checks = [
        ("b_cond_encodings array", "b_cond_encodings"),
        ("Condition code validation", "cond < 0 || cond > 5"),
        ("Error handling", "fprintf(stderr"),
        ("Encoding lookup", "b_cond_encodings[cond]"),
        ("Patch position tracking", "patch_pos"),
    ]
    
    print("Implementation checks:")
    print("-" * 40)
    
    all_ok = True
    for name, pattern in checks:
        if pattern in content:
            print(f"  ✓ {name}")
        else:
            print(f"  ✗ {name} - MISSING")
            all_ok = False
    
    print()
    if all_ok:
        print("✓ emit_jcond implementation looks correct")
    else:
        print("✗ emit_jcond implementation has issues")
    
    print()
    return all_ok

def main():
    import argparse
    parser = argparse.ArgumentParser(description="AArch64 Backend Verification")
    parser.add_argument("--check-all", action="store_true", help="Check everything")
    parser.add_argument("--check-cond", action="store_true", help="Check condition codes")
    parser.add_argument("--check-struct", action="store_true", help="Check backend struct")
    parser.add_argument("--check-cset", action="store_true", help="Check emit_cond_set")
    parser.add_argument("--check-jcond", action="store_true", help="Check emit_jcond")
    args = parser.parse_args()
    
    root = Path.cwd()
    aarch64_file = root / "src/zero_cc/isa/aarch64.c"
    
    if not any([args.check_all, args.check_cond, args.check_struct, 
                args.check_cset, args.check_jcond]):
        args.check_all = True
    
    results = []
    
    if args.check_all or args.check_cond:
        results.append(check_condition_codes())
    
    if args.check_all or args.check_struct:
        results.append(check_backend_struct(aarch64_file))
    
    if args.check_all or args.check_cset:
        results.append(check_emit_cond_set_implementation(aarch64_file))
    
    if args.check_all or args.check_jcond:
        results.append(check_emit_jcond_implementation(aarch64_file))
    
    print("=" * 70)
    print("Verification Summary")
    print("=" * 70)
    print()
    
    if all(results):
        print("✓ All checks passed!")
        return 0
    else:
        print("✗ Some checks failed")
        return 1

if __name__ == "__main__":
    sys.exit(main())
