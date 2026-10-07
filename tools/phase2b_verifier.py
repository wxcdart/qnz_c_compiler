#!/usr/bin/env python3
"""
Phase 2B Verification Tool

Verifies that all SETcc opcodes have been replaced with CMP_BRANCH.

Usage:
    python3 tools/phase2b_verifier.py

Exit codes:
    0 - All SETcc opcodes removed (Phase 2B complete)
    1 - SETcc opcodes still present (Phase 2B incomplete)
"""

import re
import sys
from pathlib import Path

SETCC_OPCODES = [
    'OP_TEST',
    'OP_SETL', 'OP_SETLE', 'OP_SETG', 'OP_SETGE',
    'OP_SET_E', 'OP_SET_NE',
]

def check_setcc_removed():
    """Check if SETcc opcodes have been removed from IR layer."""
    root = Path.cwd()
    
    files_to_check = [
        'src/zero_cc/parser/ir.h',
        'src/zero_cc/parser/ast_lowering_mc.c',
        'src/zero_cc/parser/ast_lowering_expr.c',
        'src/zero_cc/parser/ast_lowering_stmt.c',
        'src/zero_cc/parser/opt_cse.c',
        'src/zero_cc/parser/opt_dce.c',
    ]
    
    found = []
    
    for filepath_str in files_to_check:
        filepath = root / filepath_str
        if not filepath.exists():
            continue
        
        with open(filepath, 'r') as f:
            content = f.read()
        
        for opcode in SETCC_OPCODES:
            if opcode in content:
                found.append((filepath_str, opcode))
    
    return found

def check_cmp_branch_added():
    """Check if OP_CMP_BRANCH has been added."""
    root = Path.cwd()
    
    ir_file = root / 'src/zero_cc/parser/ir.h'
    if not ir_file.exists():
        return False
    
    with open(ir_file, 'r') as f:
        content = f.read()
    
    return 'OP_CMP_BRANCH' in content

def check_backend_interface_updated():
    """Check if backend interface has emit_cmp_branch."""
    root = Path.cwd()
    
    backend_file = root / 'src/zero_cc/isa/isa_backend.h'
    if not backend_file.exists():
        return False
    
    with open(backend_file, 'r') as f:
        content = f.read()
    
    return 'emit_cmp_branch' in content

def main():
    print("=" * 80)
    print("Phase 2B Verification")
    print("=" * 80)
    print()
    
    # Check 1: SETcc opcodes removed
    print("Check 1: SETcc opcodes removed from IR layer")
    print("-" * 80)
    
    found = check_setcc_removed()
    if found:
        print(f"  ✗ FAIL: Found {len(found)} SETcc opcode references:")
        for file, opcode in found:
            print(f"    - {file}: {opcode}")
        print()
        check1_pass = False
    else:
        print("  ✓ PASS: No SETcc opcodes found in IR layer")
        print()
        check1_pass = True
    
    # Check 2: CMP_BRANCH added
    print("Check 2: OP_CMP_BRANCH opcode added")
    print("-" * 80)
    
    if check_cmp_branch_added():
        print("  ✓ PASS: OP_CMP_BRANCH found in ir.h")
        print()
        check2_pass = True
    else:
        print("  ✗ FAIL: OP_CMP_BRANCH not found in ir.h")
        print()
        check2_pass = False
    
    # Check 3: Backend interface updated
    print("Check 3: Backend interface has emit_cmp_branch()")
    print("-" * 80)
    
    if check_backend_interface_updated():
        print("  ✓ PASS: emit_cmp_branch() found in isa_backend.h")
        print()
        check3_pass = True
    else:
        print("  ✗ FAIL: emit_cmp_branch() not found in isa_backend.h")
        print()
        check3_pass = False
    
    # Summary
    print("=" * 80)
    print("Summary")
    print("=" * 80)
    
    all_pass = check1_pass and check2_pass and check3_pass
    
    if all_pass:
        print("✓ Phase 2B COMPLETE - All checks passed!")
        print()
        print("Next steps:")
        print("  1. Run: make test (118/118 must pass)")
        print("  2. Run: python3 tools/isa_diff.py test/abi/comparison.c --isas x86_64 aarch64")
        return 0
    else:
        print("✗ Phase 2B INCOMPLETE - Some checks failed")
        print()
        if not check1_pass:
            print("  - Still have SETcc opcodes in IR layer")
        if not check2_pass:
            print("  - OP_CMP_BRANCH not added to ir.h")
        if not check3_pass:
            print("  - Backend interface not updated")
        return 1

if __name__ == "__main__":
    sys.exit(main())
