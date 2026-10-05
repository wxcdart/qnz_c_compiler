#!/usr/bin/env python3
"""Task 57 Bug Analysis - Diagnose optimization pass issues

Analyzes:
1. Constant folding IR corruption
2. DCE side-effect analysis issues  
3. CSE hash table integration problems
"""

import os
import re
from collections import defaultdict

def analyze_constant_folding():
    """Analyze constant folding bugs"""
    print("=" * 70)
    print("1. CONSTANT FOLDING BUG ANALYSIS")
    print("=" * 70)
    
    issues = []
    
    # Check ir_replace_with_const implementation
    opt_cf_path = 'src/zero_cc/parser/opt_constant_fold.c'
    if os.path.exists(opt_cf_path):
        with open(opt_cf_path, 'r') as f:
            content = f.read()
            
        # Check for OP_MOV_IMM usage
        if 'OP_MOV_IMM' in content:
            issues.append("⚠️  Uses OP_MOV_IMM - may not preserve all instruction fields")
        
        # Check for temp_src1/temp_src2 handling
        if 'temp_src1 = 0xFF' in content and 'temp_src2 = 0xFF' in content:
            issues.append("⚠️  Sets src1/src2 to 0xFF - may break instructions expecting valid temps")
        
        # Check for dest preservation
        if 'temp_dest = instr->temp_dest' in content:
            issues.append("✅ Preserves temp_dest")
        else:
            issues.append("⚠️  May not preserve temp_dest correctly")
    
    # Check IR opcode structure
    ir_h_path = 'src/zero_cc/parser/ir.h'
    if os.path.exists(ir_h_path):
        with open(ir_h_path, 'r') as f:
            content = f.read()
            
        # Check MOV_IMM opcode
        if 'OP_MOV_IMM' in content:
            # Find MOV_IMM definition
            match = re.search(r'OP_MOV_IMM,\s*/\*(.*)\*/', content)
            if match:
                issues.append(f"ℹ️  OP_MOV_IMM: {match.group(1).strip()}")
    
    print("\nPotential Issues:")
    for issue in issues:
        print(f"  {issue}")
    
    if not issues:
        print("  No obvious issues found")
    
    return len(issues)

def analyze_dce():
    """Analyze DCE bugs"""
    print("\n" + "=" * 70)
    print("2. DCE BUG ANALYSIS")
    print("=" * 70)
    
    issues = []
    
    opt_dce_path = 'src/zero_cc/parser/opt_dce.c'
    if os.path.exists(opt_dce_path):
        with open(opt_dce_path, 'r') as f:
            content = f.read()
            
        # Check side-effect analysis
        if 'instr_has_side_effects' in content:
            issues.append("✅ Has side-effect analysis")
            
            # Check for VLA_ALLOC
            if 'OP_VLA_ALLOC' in content:
                issues.append("✅ Handles OP_VLA_ALLOC")
            else:
                issues.append("⚠️  May not handle OP_VLA_ALLOC")
            
            # Check for atomic ops
            if 'OP_ATOMIC' in content:
                issues.append("✅ Handles atomic operations")
            else:
                issues.append("⚠️  May not handle atomic operations")
        
        # Check liveness analysis
        if 'mark_live' in content and 'is_live' in content:
            issues.append("✅ Has liveness tracking")
        
        # Check for backward pass
        if 'count - 1' in content and 'i >= 0' in content:
            issues.append("✅ Implements backward pass for liveness")
        
        # Check for IR compaction
        if 'write_idx' in content and 'instr_count =' in content:
            issues.append("✅ Compacts IR after removal")
    
    print("\nAnalysis:")
    for issue in issues:
        print(f"  {issue}")
    
    return len([i for i in issues if '⚠️' in i])

def analyze_cse():
    """Analyze CSE bugs"""
    print("\n" * 70)
    print("3. CSE BUG ANALYSIS")
    print("=" * 70)
    
    issues = []
    
    opt_cse_path = 'src/zero_cc/parser/opt_cse.c'
    if os.path.exists(opt_cse_path):
        with open(opt_cse_path, 'r') as f:
            content = f.read()
            
        # Check hash table usage
        if 'HashT' in content or 'ht32' in content:
            issues.append("✅ Uses hash table")
            
            # Check for proper API usage
            if 'ht32_put' in content and 'ht32_get' in content:
                issues.append("✅ Uses correct hash table API")
            else:
                issues.append("⚠️  May have hash table API issues")
        else:
            issues.append("⚠️  May not use hash table correctly")
        
        # Check for instruction comparison
        if 'cse_equal' in content:
            issues.append("✅ Has instruction equality check")
        
        # Check for pure expression detection
        if 'instr_is_pure' in content:
            issues.append("✅ Has pure expression detection")
        else:
            issues.append("⚠️  May not detect pure expressions")
    
    print("\nAnalysis:")
    for issue in issues:
        print(f"  {issue}")
    
    return len([i for i in issues if '⚠️' in i])

def suggest_fixes():
    """Suggest fixes for each pass"""
    print("\n" + "=" * 70)
    print("4. SUGGESTED FIXES")
    print("=" * 70)
    
    print("\nConstant Folding:")
    print("  1. Don't replace instructions - only fold when result is used")
    print("  2. Add OP_COPY opcode for temp-to-temp moves")
    print("  3. Track constants more carefully through the IR")
    print("  4. Consider two-pass approach: collect then fold")
    
    print("\nDCE:")
    print("  1. Improve side-effect analysis for all opcodes")
    print("  2. Handle memory operations (LOAD/STORE) correctly")
    print("  3. Consider control flow (jumps, calls)")
    print("  4. Add debug logging to see what's being removed")
    
    print("\nCSE:")
    print("  1. Fix hash table key encoding")
    print("  2. Ensure proper instruction comparison")
    print("  3. Handle temp renaming after CSE")
    print("  4. Consider using instruction indices instead of temps")

def main():
    print("=" * 70)
    print("Task 57 Bug Analysis")
    print("=" * 70)
    print()
    
    cf_issues = analyze_constant_folding()
    dce_issues = analyze_dce()
    cse_issues = analyze_cse()
    
    suggest_fixes()
    
    print("\n" + "=" * 70)
    print("SUMMARY")
    print("=" * 70)
    print(f"Constant folding issues: ~{cf_issues}")
    print(f"DCE issues: ~{dce_issues}")
    print(f"CSE issues: ~{cse_issues}")
    print("\nRecommendation: Fix constant folding first (simplest), then DCE, then CSE")

if __name__ == '__main__':
    main()
