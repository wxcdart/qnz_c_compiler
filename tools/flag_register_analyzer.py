#!/usr/bin/env python3
"""
Flag Register Analyzer

Analyzes current IR to determine what changes are needed for adding
explicit flag registers (flags_in, flags_out fields).

Usage:
    python3 tools/flag_register_analyzer.py

Output:
    - List of opcodes that need flags_out set
    - List of opcodes that need flags_in set
    - Instr structure size impact analysis
"""

import re
import sys
from pathlib import Path

# Opcodes that set flags
FLAG_SETTERS = {
    'OP_CMP': 'flags_out = REG_FLAGS',
    'OP_CMP_ZERO': 'flags_out = REG_FLAGS',
    'OP_TEST': 'flags_out = REG_FLAGS',
}

# Opcodes that read flags  
FLAG_READERS = {
    'OP_COND_SET': 'flags_in = REG_FLAGS',
    'OP_JCOND': 'flags_in = REG_FLAGS',  # After Phase 1
}

# Opcodes that don't use flags
FLAG_NEUTRAL = {
    'OP_NOP', 'OP_MOV', 'OP_MOV_IMM', 'OP_MOV_REG', 'OP_MOV_MEM',
    'OP_ADD', 'OP_SUB', 'OP_MUL', 'OP_DIV', 'OP_MOD',
    'OP_AND', 'OP_OR', 'OP_XOR', 'OP_NOT', 'OP_LNOT',
    'OP_SHL', 'OP_SHR', 'OP_SAR',
    'OP_NEG', 'OP_INC', 'OP_DEC',
    'OP_PUSH', 'OP_POP', 'OP_LEA',
    'OP_JMP', 'OP_JMP_LABEL',
    'OP_CALL', 'OP_CALL_INDIRECT', 'OP_RET',
    'OP_PROLOGUE', 'OP_EPILOGUE', 'OP_DEAD',
    'OP_LABEL', 'OP_DEREF',
    'OP_FADD', 'OP_FSUB', 'OP_FMUL', 'OP_FDIV',
    # ... and many more
}

def count_opcode_usage(filepath):
    """Count how many times each opcode is used in a file."""
    counts = {}
    
    with open(filepath, 'r') as f:
        content = f.read()
    
    for opcode in list(FLAG_SETTERS.keys()) + list(FLAG_READERS.keys()):
        count = len(re.findall(rf'\b{opcode}\b', content))
        if count > 0:
            counts[opcode] = count
    
    return counts

def analyze_struct_size():
    """Analyze the impact on Instr structure size."""
    print("=" * 80)
    print("Instr Structure Size Analysis")
    print("=" * 80)
    print()
    
    print("Current structure (16 bytes):")
    print("-" * 40)
    print('''
    typedef struct __attribute__((packed)) {
        uint8_t  op;        // 1 byte
        uint8_t  temp_dest; // 1 byte
        uint8_t  temp_src1; // 1 byte
        uint8_t  temp_src2; // 1 byte
        uint8_t  cond;      // 1 byte
        int16_t  imm;       // 2 bytes
        int64_t  imm64;     // 8 bytes
        uint16_t label_idx; // 2 bytes
    } Instr;  // Total: 16 bytes
    ''')
    print()
    
    print("New structure with flag registers (18 bytes):")
    print("-" * 40)
    print('''
    typedef struct __attribute__((packed)) {
        uint8_t  op;        // 1 byte
        uint8_t  temp_dest; // 1 byte
        uint8_t  temp_src1; // 1 byte
        uint8_t  temp_src2; // 1 byte
        uint8_t  cond;      // 1 byte
        uint8_t  flags_out; // 1 byte  <- NEW
        uint8_t  flags_in;  // 1 byte  <- NEW
        int16_t  imm;       // 2 bytes
        int64_t  imm64;     // 8 bytes
        uint16_t label_idx; // 2 bytes
    } Instr;  // Total: 18 bytes
    ''')
    print()
    
    print("Impact:")
    print("-" * 40)
    print("  Size increase: 16 -> 18 bytes (+12.5%)")
    print("  Memory usage: +12.5% for IR instruction buffer")
    print("  Cache impact: Minimal (still cache-line friendly)")
    print()

def main():
    root = Path.cwd()
    
    print("=" * 80)
    print("Flag Register Analyzer")
    print("=" * 80)
    print()
    
    # Analyze structure size impact
    analyze_struct_size()
    
    # Count opcode usage
    print("Opcode Usage Analysis")
    print("-" * 40)
    
    files_to_scan = [
        'src/zero_cc/parser/ast_lowering_expr.c',
        'src/zero_cc/parser/ast_lowering_mc.c',
        'src/zero_cc/parser/ir_emit_ops.h',
    ]
    
    total_setters = {}
    total_readers = {}
    
    for filepath in files_to_scan:
        full_path = root / filepath
        if full_path.exists():
            counts = count_opcode_usage(full_path)
            for opcode, count in counts.items():
                if opcode in FLAG_SETTERS:
                    total_setters[opcode] = total_setters.get(opcode, 0) + count
                elif opcode in FLAG_READERS:
                    total_readers[opcode] = total_readers.get(opcode, 0) + count
    
    print("Flag-setting opcodes (need flags_out = REG_FLAGS):")
    for opcode, count in sorted(total_setters.items()):
        print(f"  {opcode}: {count} uses")
    print()
    
    print("Flag-reading opcodes (need flags_in = REG_FLAGS):")
    for opcode, count in sorted(total_readers.items()):
        print(f"  {opcode}: {count} uses")
    print()
    
    print("Migration Checklist")
    print("-" * 40)
    print("Phase 2 tasks:")
    print("  [ ] Add REG_FLAGS constant (255)")
    print("  [ ] Update Instr structure (+2 bytes)")
    print("  [ ] Update ir_emit_cmp() - set flags_out")
    print("  [ ] Update ir_emit_cond_set() - set flags_in")
    print("  [ ] Update ir_emit_jcond() - set flags_in")
    print("  [ ] Update all backends to read flags_in/flags_out")
    print("  [ ] Implement flag CSE optimization")
    print("  [ ] Implement flag DCE optimization")
    print("  [ ] Test all 118 JIT tests")
    print()
    
    return 0

if __name__ == "__main__":
    sys.exit(main())
