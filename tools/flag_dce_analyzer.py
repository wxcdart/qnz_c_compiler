#!/usr/bin/env python3
"""
Flag DCE Analyzer

Analyzes IR to identify potential dead flag operations (CMP instructions
whose flags are never used).

This tool helps:
1. Understand the current flag usage patterns
2. Estimate DCE effectiveness
3. Verify DCE implementation correctness

Usage:
    python3 tools/flag_dce_analyzer.py [source_file]

If no source file given, analyzes all IR emission code.
"""

import re
import sys
from pathlib import Path
from collections import defaultdict

# Flag-setting opcodes
FLAG_SETTERS = {'OP_CMP', 'OP_CMP_ZERO'}

# Flag-reading opcodes
FLAG_READERS = {'OP_COND_SET', 'OP_JCOND'}

# Control flow boundaries (stop DCE analysis)
CONTROL_FLOW = {'OP_JMP', 'OP_JE', 'OP_JNE', 'OP_JL', 'OP_JLE', 'OP_JG', 
                'OP_JCOND', 'OP_JMP_LABEL', 'OP_CALL', 'OP_RET', 'OP_LABEL'}

def analyze_file(filepath):
    """Analyze a file for flag usage patterns."""
    results = {
        'flag_setters': [],
        'flag_readers': [],
        'potential_dead_cmp': [],
        'flag_usage_pairs': [],
    }
    
    try:
        with open(filepath, 'r') as f:
            content = f.read()
            lines = content.split('\n')
    except Exception as e:
        return None
    
    # Find flag setter calls
    for i, line in enumerate(lines, 1):
        for opcode in FLAG_SETTERS:
            if opcode.lower() in line.lower() and 'ir_emit' in line.lower():
                results['flag_setters'].append({
                    'line': i,
                    'content': line.strip(),
                    'opcode': opcode,
                })
        
        for opcode in FLAG_READERS:
            if opcode.lower() in line.lower() and 'ir_emit' in line.lower():
                results['flag_readers'].append({
                    'line': i,
                    'content': line.strip(),
                    'opcode': opcode,
                })
    
    return results

def estimate_dce_effectiveness(all_results):
    """Estimate how effective DCE would be."""
    total_setters = sum(len(r['flag_setters']) for r in all_results.values() if r)
    total_readers = sum(len(r['flag_readers']) for r in all_results.values() if r)
    
    print("=" * 80)
    print("Flag DCE Effectiveness Estimate")
    print("=" * 80)
    print()
    print(f"Total flag-setting operations (CMP): {total_setters}")
    print(f"Total flag-reading operations:       {total_readers}")
    print()
    
    # Calculate ratio
    if total_setters > 0:
        ratio = total_readers / total_setters
        print(f"Reader/Setter ratio: {ratio:.2f}")
        print()
        
        if ratio >= 1.0:
            print("Assessment: HIGH flag usage")
            print("  - Most CMP results are used")
            print("  - DCE effectiveness: LOW (few dead CMPs)")
        elif ratio >= 0.5:
            print("Assessment: MODERATE flag usage")
            print("  - Some CMP results may be unused")
            print("  - DCE effectiveness: MEDIUM")
        else:
            print("Assessment: LOW flag usage")
            print("  - Many CMP results may be unused")
            print("  - DCE effectiveness: HIGH")
    print()

def generate_dce_plan():
    """Generate DCE implementation plan."""
    print("=" * 80)
    print("Flag DCE Implementation Plan")
    print("=" * 80)
    print()
    
    print("Algorithm:")
    print("-" * 40)
    print("""
void opt_flag_dce(ZeroCCCompiler *c) {
    for (int i = 0; i < instr_count; i++) {
        Instr *instr = &instrs[i];
        
        // Only analyze flag-setting instructions
        if (instr->op != OP_CMP && instr->op != OP_CMP_ZERO) {
            continue;
        }
        
        // Check if flags are used by subsequent instructions
        bool flags_used = false;
        for (int j = i + 1; j < instr_count; j++) {
            Instr *next = &instrs[j];
            
            // Stop at control flow boundary
            if (next->op == OP_JMP || next->op == OP_JCOND || 
                next->op == OP_RET || next->op == OP_LABEL) {
                break;
            }
            
            // Check if this instruction reads flags
            if (next->flags_in == REG_FLAGS) {
                flags_used = true;
                break;
            }
        }
        
        // Mark as dead if flags never used
        if (!flags_used) {
            instr->op = OP_DEAD;
        }
    }
}
    """)
    print()
    
    print("Integration into optimization pipeline:")
    print("-" * 40)
    print("""
// In opt_pass.c or similar:
opt_passes[] = {
    { "constant_fold", opt_constant_fold },
    { "flag_dce",      opt_flag_dce },      // NEW
    { "cse",           opt_cse },
    { "dce",           opt_dce },           // General DCE removes OP_DEAD
};
    """)
    print()

def generate_test_cases():
    """Generate test cases for DCE verification."""
    print("=" * 80)
    print("Test Cases for Flag DCE")
    print("=" * 80)
    print()
    
    print("Test 1: Dead CMP (should be removed)")
    print("-" * 40)
    print("""
    // Before:
    CMP a, b      // flags_out = REG_FLAGS
    ADD x, y      // flags_in = 0xFF (doesn't use flags)
    
    // After DCE:
    ADD x, y
    """)
    print()
    
    print("Test 2: Live CMP (should be kept)")
    print("-" * 40)
    print("""
    // Before:
    CMP a, b      // flags_out = REG_FLAGS
    COND_SET x, EQ  // flags_in = REG_FLAGS
    
    // After DCE: (unchanged)
    CMP a, b
    COND_SET x, EQ
    """)
    print()
    
    print("Test 3: CMP across control flow (should be kept)")
    print("-" * 40)
    print("""
    // Before:
    CMP a, b      // flags_out = REG_FLAGS
    JCOND eq, L1  // flags_in = REG_FLAGS
    ADD x, y
    LABEL L1:
    
    // After DCE: (unchanged - CMP is used by jump)
    CMP a, b
    JCOND eq, L1
    ADD x, y
    LABEL L1:
    """)
    print()
    
    print("Test 4: Multiple CMPs, some dead")
    print("-" * 40)
    print("""
    // Before:
    CMP a, b      // dead - no user
    ADD x, y
    CMP c, d      // live - used by COND_SET
    COND_SET z, GT
    
    // After DCE:
    ADD x, y
    CMP c, d
    COND_SET z, GT
    """)
    print()

def main():
    root = Path.cwd()
    
    print("=" * 80)
    print("Flag DCE Analyzer")
    print("=" * 80)
    print()
    
    # Files to analyze
    files_to_scan = [
        'src/zero_cc/parser/ast_lowering_expr.c',
        'src/zero_cc/parser/ast_lowering_stmt.c',
        'src/zero_cc/parser/ir_emit_ops.h',
    ]
    
    all_results = {}
    for filepath in files_to_scan:
        full_path = root / filepath
        if full_path.exists():
            results = analyze_file(full_path)
            all_results[filepath] = results
    
    # Estimate effectiveness
    estimate_dce_effectiveness(all_results)
    
    # Generate implementation plan
    generate_dce_plan()
    
    # Generate test cases
    generate_test_cases()
    
    print("=" * 80)
    print("Analysis Complete")
    print("=" * 80)
    print()
    print("Next steps:")
    print("1. Review the implementation plan above")
    print("2. Implement opt_flag_dce() in a new file or opt_dce.c")
    print("3. Add to optimization pipeline")
    print("4. Test with existing 118 JIT tests")
    print("5. Add new test cases if needed")
    print()
    
    return 0

if __name__ == "__main__":
    sys.exit(main())
