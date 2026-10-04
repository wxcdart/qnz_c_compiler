#!/usr/bin/env python3
"""Task 64 Detailed Analyzer - Identify remaining ISA-specific patterns in IR layer"""

import re
import os
import sys

# Files that should NOT have ISA-specific code (IR layer files)
IR_LAYER_FILES = [
    'src/zero_cc/parser/ast_lowering.c',
    'src/zero_cc/parser/ast_lowering_expr.c',
    'src/zero_cc/parser/ast_lowering_stmt.c',
    'src/zero_cc/parser/ast_lowering_ir.h',
    'src/zero_cc/parser/ast_lowering_mc.c',
    'src/zero_cc/parser/ir.h',
    'src/zero_cc/parser/ir_emit_ops.h',
    'src/zero_cc/parser/ir_emitter.h',
    'src/zero_cc/parser/regalloc.h',
    'src/zero_cc/parser/live_range.h',
    'src/zero_cc/parser/abi_classify.h',
    'src/zero_cc/parser/const_eval.h',
    'src/zero_cc/parser/ast_expr.h',
    'src/zero_cc/parser/ast_stmt.h',
]

# Patterns that indicate ISA-specific code (should be in backend, not IR layer)
ISA_PATTERNS = {
    'hardcoded_registers': [
        (r'\brax\b', 'Hardcoded x86 register rax'),
        (r'\brbx\b', 'Hardcoded x86 register rbx'),
        (r'\brbp\b', 'Hardcoded x86 register rbp'),
        (r'\brsp\b', 'Hardcoded x86 register rsp'),
        (r'\brdi\b', 'Hardcoded x86 register rdi'),
        (r'\brsi\b', 'Hardcoded x86 register rsi'),
        (r'\brcx\b', 'Hardcoded x86 register rcx'),
        (r'\brdx\b', 'Hardcoded x86 register rdx'),
        (r'\bxmm[0-9]+\b', 'Hardcoded x86 FP register'),
    ],
    'isa_backend_calls': [
        (r'BE\(c\)->emit_', 'Backend emit call (check if appropriate)'),
        (r'BE\(c\)->fp_reg', 'Backend FP register access'),
        (r'BE\(c\)->sp_reg', 'Backend SP register access'),
    ],
    'machine_emitter_calls': [
        (r'\bme_emit_', 'Machine emitter call (should be in backend)'),
        (r'\bme_get_offset\b', 'Machine emitter offset (should use backend)'),
    ],
    'x86_opcodes': [
        (r'0x48\s*,\s*0x[Bb][0-9A-Fa-f]{2}', 'x86 REX prefix opcode'),
        (r'0x89\b', 'x86 MOV opcode'),
        (r'0x8B\b', 'x86 MOV opcode'),
        (r'0xC9\b', 'x86 LEAVE opcode'),
        (r'0xC3\b', 'x86 RET opcode'),
        (r'0xE9\b', 'x86 JMP opcode'),
    ],
}

def analyze_file(filepath):
    """Analyze a single file for ISA-specific patterns"""
    if not os.path.exists(filepath):
        return None
    
    results = {pattern_name: [] for pattern_name in ISA_PATTERNS}
    
    try:
        with open(filepath, 'r') as f:
            lines = f.readlines()
    except Exception as e:
        return None
    
    for line_num, line in enumerate(lines, 1):
        # Skip comments
        if line.strip().startswith('//') or line.strip().startswith('/*'):
            continue
            
        for pattern_name, patterns in ISA_PATTERNS.items():
            for pattern, description in patterns:
                if re.search(pattern, line):
                    results[pattern_name].append({
                        'line': line_num,
                        'content': line.strip()[:80],
                        'description': description
                    })
    
    return results

def main():
    print("=" * 70)
    print("Task 64 Detailed Analyzer - IR Layer ISA-Specific Patterns")
    print("=" * 70)
    
    total_issues = 0
    
    for filepath in IR_LAYER_FILES:
        results = analyze_file(filepath)
        if not results:
            continue
        
        file_issues = sum(len(v) for v in results.values())
        if file_issues == 0:
            continue
            
        print(f"\n{filepath}:")
        print(f"  Issues found: {file_issues}")
        
        for pattern_name, issues in results.items():
            if issues:
                print(f"  [{pattern_name}] {len(issues)} occurrences:")
                for issue in issues[:5]:  # Show first 5
                    print(f"    Line {issue['line']}: {issue['content']}")
                if len(issues) > 5:
                    print(f"    ... and {len(issues) - 5} more")
        
        total_issues += file_issues
    
    print("\n" + "=" * 70)
    print(f"Total issues in IR layer: {total_issues}")
    print("=" * 70)
    
    if total_issues == 0:
        print("\n✅ IR layer is clean - no ISA-specific patterns found!")
        print("\nRemaining work for Task 64:")
        print("  - The isa_backend_calls are CORRECT usage (using BE(c)->...)")
        print("  - machine_emitter.h is only used by ISA backends now")
        print("  - Consider creating canonical IR ops for common patterns:")
        print("    - OP_PUSH_FRAME / OP_ALLOC_STACK for stack management")
        print("    - OP_LOAD_SYMBOL / OP_ADDR_OF for symbol addressing")
        print("    - IR layer is already target-agnostic")

if __name__ == '__main__':
    main()
