#!/usr/bin/env python3
"""
Flag Register Migration Tool

Prepares for Phase 2: Add explicit flag registers to IR.

Generates:
1. Instr structure size analysis
2. List of all places needing flags_in/flags_out updates
3. Migration checklist with line-by-line instructions
4. Backend update templates

Usage:
    python3 tools/flag_register_migration.py
"""

import re
import sys
from pathlib import Path
from collections import defaultdict

# Opcodes that need flags_out = REG_FLAGS
FLAG_SETTERS = {
    'OP_CMP',
    'OP_CMP_ZERO',
    'OP_TEST',  # If added later
}

# Opcodes that need flags_in = REG_FLAGS
FLAG_READERS = {
    'OP_COND_SET',
    'OP_JCOND',
}

def analyze_struct_size():
    """Analyze Instr structure size impact."""
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
    
    print("Impact:")
    print("-" * 40)
    print("  Size increase: 16 -> 18 bytes (+12.5%)")
    print("  Memory usage: +12.5% for IR instruction buffer")
    print("  Cache impact: Minimal (still cache-line friendly)")
    print()

def find_opcode_usage(filepath):
    """Find all places where flag-setting/reading opcodes are used."""
    results = {
        'flag_setters': [],
        'flag_readers': [],
        'helper_functions': [],
    }
    
    try:
        with open(filepath, 'r') as f:
            lines = f.readlines()
    except Exception as e:
        return None
    
    for i, line in enumerate(lines, 1):
        # Check for flag setters in ir_emit_* functions
        for opcode in FLAG_SETTERS:
            # Look for ir_emit patterns
            pattern = rf'ir_emit.*{opcode.lower().replace("_", "_")}'
            if re.search(pattern, line, re.IGNORECASE):
                results['flag_setters'].append((i, line.strip(), filepath))
            
            # Look for opcode in struct initialization
            if f'.op = {opcode}' in line:
                results['flag_setters'].append((i, line.strip(), filepath))
        
        # Check for flag readers
        for opcode in FLAG_READERS:
            pattern = rf'ir_emit.*{opcode.lower().replace("_", "_")}'
            if re.search(pattern, line, re.IGNORECASE):
                results['flag_readers'].append((i, line.strip(), filepath))
            
            if f'.op = {opcode}' in line:
                results['flag_readers'].append((i, line.strip(), filepath))
    
    return results

def find_helper_functions(filepath):
    """Find helper functions that emit flag operations."""
    helpers = []
    
    try:
        with open(filepath, 'r') as f:
            content = f.read()
    except Exception:
        return []
    
    # Find ir_emit_cmp, ir_emit_cond_set, etc.
    patterns = [
        r'static inline void ir_emit_cmp\([^)]+\)',
        r'static inline void ir_emit_cond_set\([^)]+\)',
        r'static inline void ir_emit_jcond\([^)]+\)',
    ]
    
    for pattern in patterns:
        matches = re.finditer(pattern, content)
        for match in matches:
            # Find line number
            line_num = content[:match.start()].count('\n') + 1
            helpers.append((line_num, match.group(0), filepath))
    
    return helpers

def generate_migration_plan(all_results):
    """Generate detailed migration plan."""
    print("=" * 80)
    print("Migration Plan")
    print("=" * 80)
    print()
    
    # Count total usages
    total_setters = sum(len(r['flag_setters']) for r in all_results.values() if r)
    total_readers = sum(len(r['flag_readers']) for r in all_results.values() if r)
    
    print(f"Flag-setting operations to update: {total_setters}")
    print(f"Flag-reading operations to update: {total_readers}")
    print()
    
    print("Step 1: Define REG_FLAGS constant")
    print("-" * 40)
    print("Add to ir.h:")
    print('  #define REG_FLAGS 255  // Special flag register')
    print()
    
    print("Step 2: Update Instr structure")
    print("-" * 40)
    print("Add to ir.h (after cond field):")
    print('  uint8_t flags_out;  // Flags output register (REG_FLAGS or 0xFF)')
    print('  uint8_t flags_in;   // Flags input register (REG_FLAGS or 0xFF)')
    print()
    
    print("Step 3: Update helper functions")
    print("-" * 40)
    
    # Find helper functions
    helper_files = [
        'src/zero_cc/parser/ir.h',
        'src/zero_cc/parser/ir_emit_ops.h',
    ]
    
    for filepath in helper_files:
        full_path = Path.cwd() / filepath
        if full_path.exists():
            helpers = find_helper_functions(full_path)
            for line, func, _ in helpers:
                if 'cmp' in func.lower() or 'cond_set' in func.lower() or 'jcond' in func.lower():
                    print(f"  {filepath}:{line} - {func[:60]}")
    
    print()
    print("Step 4: Update backend implementations")
    print("-" * 40)
    print("Backends need to read flags_in/flags_out from instructions:")
    print("  - x86_64.c: x86_emit_instr()")
    print("  - aarch64.c: aarch64_emit_instr()")
    print("  - riscv.c: riscv64_emit_instr()")
    print()
    
    print("Step 5: Implement flag optimizations")
    print("-" * 40)
    print("New optimization passes:")
    print("  - Flag CSE: Share cmp results across multiple users")
    print("  - Flag DCE: Remove cmp if flags are never used")
    print()

def generate_backend_template():
    """Generate backend implementation template."""
    print("=" * 80)
    print("Backend Implementation Template")
    print("=" * 80)
    print()
    
    print("In x86_emit_instr(), x86_emit_instr(), etc.:")
    print("-" * 40)
    print('''
    case OP_CMP:
        // instr->flags_out = REG_FLAGS (explicitly sets flags)
        x86_emit_cmp(c, instr->temp_src1, instr->temp_src2);
        break;
    
    case OP_COND_SET:
        // instr->flags_in = REG_FLAGS (explicitly reads flags)
        x86_emit_cond_set(c, instr->temp_dest, instr->cond);
        break;
    
    case OP_JCOND:
        // instr->flags_in = REG_FLAGS (explicitly reads flags)
        x86_emit_jcond(c, instr->cond, get_label(instr->label_idx));
        break;
    ''')
    print()

def main():
    root = Path.cwd()
    
    # Files to analyze
    files_to_scan = [
        'src/zero_cc/parser/ir.h',
        'src/zero_cc/parser/ir.c',
        'src/zero_cc/parser/ir_emit_ops.h',
        'src/zero_cc/parser/ast_lowering_expr.c',
        'src/zero_cc/parser/ast_lowering_stmt.c',
        'src/zero_cc/parser/ast_lowering_mc.c',
    ]
    
    print("Analyzing flag register migration requirements...")
    print()
    
    # Analyze structure size
    analyze_struct_size()
    
    # Find opcode usage
    all_results = {}
    for filepath in files_to_scan:
        full_path = root / filepath
        if full_path.exists():
            results = find_opcode_usage(full_path)
            all_results[filepath] = results
    
    # Generate migration plan
    generate_migration_plan(all_results)
    
    # Generate backend template
    generate_backend_template()
    
    print("=" * 80)
    print("Migration Checklist")
    print("=" * 80)
    print()
    print("[ ] Add REG_FLAGS constant to ir.h")
    print("[ ] Update Instr structure (+2 bytes)")
    print("[ ] Update ir_emit_cmp() - set flags_out = REG_FLAGS")
    print("[ ] Update ir_emit_cmp_zero() - set flags_out = REG_FLAGS")
    print("[ ] Update ir_emit_cond_set() - set flags_in = REG_FLAGS")
    print("[ ] Update ir_emit_jcond() - set flags_in = REG_FLAGS")
    print("[ ] Update x86_64.c backend")
    print("[ ] Update aarch64.c backend")
    print("[ ] Update riscv.c backend")
    print("[ ] Implement flag CSE optimization")
    print("[ ] Implement flag DCE optimization")
    print("[ ] Test all 118 JIT tests")
    print()
    
    return 0

if __name__ == "__main__":
    sys.exit(main())
