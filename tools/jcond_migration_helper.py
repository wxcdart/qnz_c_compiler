#!/usr/bin/env python3
"""
OP_JCOND Migration Helper

Generates a detailed migration plan for converting implicit flag-reading
jumps (OP_JE, OP_JL, etc.) to explicit OP_JCOND.

Usage:
    python3 tools/jcond_migration_helper.py

Output:
    - Line-by-line migration instructions
    - Condition code mapping table
    - Backend implementation template
"""

import re
import sys
from pathlib import Path

# Condition code mapping: OP_Jxx -> CC_XX
JCC_TO_CC = {
    'OP_JE': 'CC_EQ',
    'OP_JNE': 'CC_NE',
    'OP_JL': 'CC_LT',
    'OP_JLE': 'CC_LE',
    'OP_JG': 'CC_GT',
    'OP_JGE': 'CC_GE',
}

# Reverse mapping for backend
CC_TO_JCC = {v: k for k, v in JCC_TO_CC.items()}

def find_jump_instructions(filepath):
    """Find all jump instructions in a file."""
    jumps = []
    
    with open(filepath, 'r') as f:
        lines = f.readlines()
    
    for i, line in enumerate(lines, 1):
        for jxx, cc in JCC_TO_CC.items():
            # Look for case statements like "case OP_JE:"
            pattern = rf'case\s+{jxx}:'
            if re.search(pattern, line):
                jumps.append({
                    'line': i,
                    'opcode': jxx,
                    'condition': cc,
                    'content': line.strip(),
                })
    
    return jumps

def generate_migration_instructions(jumps):
    """Generate line-by-line migration instructions."""
    print("=" * 80)
    print("OP_JCOND Migration Instructions")
    print("=" * 80)
    print()
    
    if not jumps:
        print("No jump instructions found to migrate.")
        return []
    
    instructions = []
    
    for jump in jumps:
        inst = {
            'line': jump['line'],
            'from_opcode': jump['opcode'],
            'to_condition': jump['condition'],
            'action': f"Replace case {jump['opcode']} with case OP_JCOND and use cond={jump['condition']}",
        }
        instructions.append(inst)
        
        print(f"Line {jump['line']}:")
        print(f"  FROM: case {jump['opcode']}:")
        print(f"  TO:   case OP_JCOND:  // cond = {jump['condition']}")
        print()
    
    return instructions

def generate_backend_template():
    """Generate backend implementation template."""
    print("=" * 80)
    print("Backend Implementation Template")
    print("=" * 80)
    print()
    
    print("1. Add to isa_backend.h:")
    print("-" * 40)
    print('''
    size_t (*emit_jcond)(ZeroCCCompiler *c, int cond, const char *label);
    ''')
    print()
    
    print("2. Add to x86_64.c:")
    print("-" * 40)
    print('''
    static size_t x86_emit_jcond(ZeroCCCompiler *c, int cond, const char *label) {
        // Condition code mapping: CC_LT, CC_LE, CC_GT, CC_GE, CC_EQ, CC_NE
        static const uint8_t jcc_codes[] = {0x8C, 0x8E, 0x8F, 0x8D, 0x84, 0x85};
        
        if (cond < 0 || cond > 5) {
            fprintf(stderr, "Invalid condition code: %d\\n", cond);
            abort();
        }
        
        // jcc label (reads flags set by previous cmp)
        me_emit_u8(c, 0x0F);
        me_emit_u8(c, jcc_codes[cond]);
        return me_emit_jmp_rel(c, label);
    }
    ''')
    print()
    
    print("3. Add to x86_64 backend table:")
    print("-" * 40)
    print('''
    .emit_jcond = x86_emit_jcond,
    ''')
    print()

def generate_ast_lowering_template():
    """Generate ast_lowering_mc.c template."""
    print("=" * 80)
    print("ast_lowering_mc.c Template")
    print("=" * 80)
    print()
    
    print("Replace individual jump cases with OP_JCOND:")
    print("-" * 40)
    print('''
    // OLD:
    case OP_JE:
        BE(c)->emit_jeq(c, label);
        break;
    case OP_JL:
        BE(c)->emit_jlt(c, label);
        break;
    
    // NEW:
    case OP_JCOND:
        if (BE(c)->emit_jcond) {
            BE(c)->emit_jcond(c, (int)instr->cond, get_label_name(instr->label_idx));
        }
        break;
    ''')
    print()

def main():
    root = Path.cwd()
    
    # Files to analyze
    files_to_scan = [
        'src/zero_cc/parser/ast_lowering_mc.c',
    ]
    
    print("Finding jump instructions for migration...")
    print()
    
    all_jumps = []
    for filepath in files_to_scan:
        full_path = root / filepath
        if full_path.exists():
            jumps = find_jump_instructions(full_path)
            all_jumps.extend(jumps)
    
    # Generate migration instructions
    instructions = generate_migration_instructions(all_jumps)
    
    # Generate templates
    generate_backend_template()
    generate_ast_lowering_template()
    
    print("=" * 80)
    print("Migration Summary")
    print("=" * 80)
    print()
    print(f"Total jumps to convert: {len(all_jumps)}")
    print(f"Files affected: {len(set(j['file'] for j in all_jumps)) if all_jumps else 0}")
    print()
    print("Estimated effort: 2-3 days")
    print("Risk level: LOW (backward compatible)")
    print()
    
    return 0

if __name__ == "__main__":
    sys.exit(main())
