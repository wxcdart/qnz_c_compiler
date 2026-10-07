#!/usr/bin/env python3
"""
Opcode Usage Analyzer for Phase 2B

Finds all usages of SETcc opcodes to plan the CMP_BRANCH refactoring.

Usage:
    python3 tools/opcode_usage_analyzer.py
"""

import re
from pathlib import Path
from collections import defaultdict

SETCC_OPCODES = [
    'OP_TEST',
    'OP_SETL', 'OP_SETLE', 'OP_SETG', 'OP_SETGE',
    'OP_SET_E', 'OP_SET_NE',
    'OP_LNOT',  # Uses SETcc internally
]

def find_opcode_usages():
    """Find all usages of SETcc opcodes in the codebase."""
    root = Path.cwd()
    
    files_to_check = [
        'src/zero_cc/parser/ir.h',
        'src/zero_cc/parser/ast_lowering_mc.c',
        'src/zero_cc/parser/ast_lowering_expr.c',
        'src/zero_cc/parser/ast_lowering_stmt.c',
        'src/zero_cc/parser/opt_cse.c',
        'src/zero_cc/parser/opt_dce.c',
        'src/zero_cc/isa/x86_64.c',
        'src/zero_cc/isa/aarch64.c',
        'src/zero_cc/isa/riscv.c',
    ]
    
    usages = defaultdict(list)
    
    for filepath_str in files_to_check:
        filepath = root / filepath_str
        if not filepath.exists():
            continue
        
        with open(filepath, 'r') as f:
            lines = f.readlines()
        
        for line_num, line in enumerate(lines, 1):
            for opcode in SETCC_OPCODES:
                # Match opcode as word boundary
                if re.search(rf'\b{opcode}\b', line):
                    usages[opcode].append({
                        'file': filepath_str,
                        'line': line_num,
                        'code': line.strip()[:100]
                    })
    
    return usages

def print_usage_report(usages):
    """Print a usage report for each SETcc opcode."""
    print("=" * 80)
    print("SETcc Opcode Usage Report for Phase 2B")
    print("=" * 80)
    print()
    
    total_usages = 0
    
    for opcode in SETCC_OPCODES:
        occurrences = usages.get(opcode, [])
        total_usages += len(occurrences)
        
        print(f"{opcode}: {len(occurrences)} occurrences")
        print("-" * 80)
        
        for usage in occurrences:
            print(f"  {usage['file']}:{usage['line']}")
            print(f"    {usage['code']}")
        print()
    
    print("=" * 80)
    print(f"Total SETcc usages: {total_usages}")
    print("=" * 80)
    print()
    
    # Categorize by file type
    ir_layer = sum(len(u) for u in usages.values() if any('parser/' in str(x['file']) for x in u))
    backend_layer = sum(len(u) for u in usages.values() if any('isa/' in str(x['file']) for x in u))
    
    print(f"By layer:")
    print(f"  IR/Lowering layer: {ir_layer}")
    print(f"  Backend layer: {backend_layer}")
    print()

def print_refactoring_plan(usages):
    """Print a refactoring plan based on usage analysis."""
    print("=" * 80)
    print("Refactoring Plan for Phase 2B")
    print("=" * 80)
    print()
    
    print("Step 1: Add new OP_CMP_BRANCH opcode to ir.h")
    print("  - Add opcode enum")
    print("  - Add helper function ir_emit_cmp_branch()")
    print("  - Add condition code constants (CC_LT, CC_LE, etc.)")
    print()
    
    print("Step 2: Update backend interface (isa_backend.h)")
    print("  - Add emit_cmp_branch() function pointer")
    print("  - Add condition code defines")
    print()
    
    print("Step 3: Implement backend functions")
    print("  - x86_64.c: cmp + setcc")
    print("  - aarch64.c: cmp + cset")
    print("  - riscv.c: slt + branch")
    print()
    
    print("Step 4: Update IR lowering code")
    for opcode in SETCC_OPCODES:
        occurrences = usages.get(opcode, [])
        files = set(u['file'] for u in occurrences if 'parser/' in u['file'])
        if files:
            print(f"  - Replace {opcode} in: {', '.join(files)}")
    print()
    
    print("Step 5: Remove old SETcc opcodes")
    print("  - Remove from ir.h")
    print("  - Remove switch cases from ast_lowering_mc.c")
    print("  - Remove backend functions from x86_64.c")
    print()
    
    print("Step 6: Test and verify")
    print("  - make test (118/118 must pass)")
    print("  - python3 tools/opcode_usage_analyzer.py (should show 0 usages)")
    print()

def main():
    usages = find_opcode_usages()
    print_usage_report(usages)
    print_refactoring_plan(usages)

if __name__ == "__main__":
    main()
