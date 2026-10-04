#!/usr/bin/env python3
"""Task 64 Comment Updater - ISA-agnostic IR opcode comments

This tool identifies IR opcode comments that contain ISA-specific
register names and suggests ISA-agnostic alternatives.
"""

import re
import os

# ISA-specific patterns to find in comments
ISA_PATTERNS = [
    (r'\brax\b', 'rax → register'),
    (r'\brbx\b', 'rbx → register'),
    (r'\brbp\b', 'rbp → frame pointer'),
    (r'\brsp\b', 'rsp → stack pointer'),
    (r'\brdi\b', 'rdi → argument register'),
    (r'\brsi\b', 'rsi → argument register'),
    (r'\brcx\b', 'rcx → register'),
    (r'\brdx\b', 'rdx → register/argument'),
    (r'\bxmm[0-9]+\b', 'xmmN → FP register'),
    (r'\[rbp\s*-\s*offset\]', '[fp - offset] → stack slot'),
    (r'mov\s+rax', 'move → general move operation'),
    (r'mov\s+\[rbp', 'store to stack → store to frame'),
]

# Suggested replacements for common patterns
RECOMMENDATIONS = {
    'OP_MOV_IMM': 'OP_MOV_IMM        /* move immediate to register */',
    'OP_MOV_MEM_RAX': 'OP_MOV_MEM_RAX     /* store register to stack slot */',
    'OP_MOV_RAX_MEM': 'OP_MOV_RAX_MEM     /* load from stack slot to register */',
    'OP_LEA_RIP': 'OP_LEA_RIP         /* load effective address (RIP-relative) */',
    'OP_PROLOGUE': 'OP_PROLOGUE        /* function prologue (frame setup) */',
}

def analyze_ir_h():
    """Analyze ir.h for ISA-specific comments"""
    filepath = 'src/zero_cc/parser/ir.h'
    
    if not os.path.exists(filepath):
        print(f"File not found: {filepath}")
        return
    
    with open(filepath, 'r') as f:
        lines = f.readlines()
    
    print("=" * 70)
    print("Task 64: IR Opcode Comment Analysis")
    print("=" * 70)
    print(f"\nAnalyzing: {filepath}\n")
    
    issues = []
    
    for i, line in enumerate(lines, 1):
        # Only check lines with /* comments
        if '/*' not in line:
            continue
            
        for pattern, suggestion in ISA_PATTERNS:
            if re.search(pattern, line, re.IGNORECASE):
                issues.append({
                    'line': i,
                    'content': line.strip()[:80],
                    'pattern': pattern,
                    'suggestion': suggestion
                })
                break
    
    if issues:
        print(f"Found {len(issues)} ISA-specific comment patterns:\n")
        for issue in issues:
            print(f"Line {issue['line']}:")
            print(f"  Current: {issue['content']}")
            print(f"  Issue: {issue['suggestion']}")
            print()
    else:
        print("No ISA-specific patterns found in comments!")
    
    print("\n" + "=" * 70)
    print("Recommended IR Opcode Comment Updates")
    print("=" * 70)
    print()
    
    for opcode, suggestion in RECOMMENDATIONS.items():
        print(f"{suggestion}")

def generate_updated_ir_h():
    """Generate suggestions for updating ir.h comments"""
    return '''
// Suggested updates for ir.h opcode comments:

// OLD: OP_MOV_IMM,        /* mov rax, imm16 */
// NEW: OP_MOV_IMM,        /* move immediate value to register */

// OLD: OP_MOV_MEM_RAX,    /* mov [rbp - offset], rax */
// NEW: OP_MOV_MEM_RAX,    /* store register to stack slot */

// OLD: OP_MOV_RAX_MEM,    /* mov rax, [rbp - offset] */
// NEW: OP_MOV_RAX_MEM,    /* load from stack slot to register */

// OLD: OP_LEA_RIP,        /* lea rax, [rip + offset] */
// NEW: OP_LEA_RIP,        /* load effective address (RIP-relative) */

// OLD: OP_PROLOGUE,       /* function prologue */
// NEW: OP_PROLOGUE,       /* function prologue (frame setup) */

// OLD: OP_PARAM_SAVE_INT, /* save int arg reg to stack */
// NEW: OP_PARAM_SAVE_INT, /* save integer argument register to stack slot */

// OLD: OP_PARAM_SAVE_FP,  /* save fp arg reg to stack */
// NEW: OP_PARAM_SAVE_FP,  /* save floating-point argument register to stack slot */
'''

def main():
    analyze_ir_h()
    print("\n" + generate_updated_ir_h())

if __name__ == '__main__':
    main()
