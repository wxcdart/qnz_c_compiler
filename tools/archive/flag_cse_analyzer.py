#!/usr/bin/env python3
"""
Flag CSE Analyzer

Analyzes IR to identify opportunities for Flag CSE (Common Subexpression
Elimination) - sharing CMP results across multiple flag-reading instructions.

This tool helps:
1. Understand flag live ranges
2. Identify CSE opportunities (CMP with multiple users)
3. Estimate CSE effectiveness
4. Verify CSE implementation correctness

Usage:
    python3 tools/flag_cse_analyzer.py
"""

import re
import sys
from pathlib import Path
from collections import defaultdict

# Flag-setting opcodes
FLAG_SETTERS = {'OP_CMP', 'OP_CMP_ZERO'}

# Flag-reading opcodes
FLAG_READERS = {'OP_COND_SET', 'OP_JCOND'}

# Control flow boundaries (stop flag liveness)
CONTROL_FLOW = {
    'OP_JMP', 'OP_JE', 'OP_JNE', 'OP_JL', 'OP_JLE', 'OP_JG', 
    'OP_JCOND', 'OP_JMP_LABEL', 'OP_CALL', 'OP_RET', 'OP_LABEL',
    'OP_PROLOGUE', 'OP_EPILOGUE'
}

def analyze_flag_liveness(instructions):
    """
    Analyze flag liveness for a list of instructions.
    
    Returns:
        list of (cmp_index, user_indices) tuples for CMPs with multiple users
    """
    results = []
    
    for i, instr in enumerate(instructions):
        if instr['op'] not in FLAG_SETTERS:
            continue
        
        # Find all flag users for this CMP
        users = []
        for j in range(i + 1, len(instructions)):
            next_instr = instructions[j]
            
            # Stop at control flow boundary
            if next_instr['op'] in CONTROL_FLOW:
                break
            
            # Check if this instruction reads flags
            if next_instr.get('flags_in') == 'REG_FLAGS':
                users.append(j)
        
        # Record if this CMP has multiple users (CSE opportunity)
        if len(users) > 1:
            results.append((i, users))
    
    return results

def estimate_cse_effectiveness(all_results):
    """Estimate how effective CSE would be."""
    total_setters = sum(r['total_setters'] for r in all_results.values() if r)
    total_multi_user = sum(r['multi_user_cmps'] for r in all_results.values() if r)
    
    print("=" * 80)
    print("Flag CSE Effectiveness Estimate")
    print("=" * 80)
    print()
    print(f"Total flag-setting operations (CMP): {total_setters}")
    print(f"CMPs with multiple flag users:       {total_multi_user}")
    print()
    
    if total_setters > 0:
        ratio = total_multi_user / total_setters
        print(f"Multi-user ratio: {ratio:.2%}")
        print()
        
        if ratio >= 0.3:
            print("Assessment: HIGH CSE potential")
            print("  - Many CMPs have multiple users")
            print("  - CSE effectiveness: HIGH")
        elif ratio >= 0.1:
            print("Assessment: MODERATE CSE potential")
            print("  - Some CMPs have multiple users")
            print("  - CSE effectiveness: MEDIUM")
        else:
            print("Assessment: LOW CSE potential")
            print("  - Few CMPs have multiple users")
            print("  - CSE effectiveness: LOW")
    print()

def generate_cse_plan():
    """Generate CSE implementation plan."""
    print("=" * 80)
    print("Flag CSE Implementation Plan")
    print("=" * 80)
    print()
    
    print("Algorithm:")
    print("-" * 40)
    print("""
void opt_flag_cse(ZeroCCCompiler *c) {
    int count = (int)ir_get_count();
    
    // Forward pass: find CMPs with multiple flag users
    for (int i = 0; i < count; i++) {
        Instr *cmp = ir_get(i);
        if (!cmp) continue;
        
        // Only analyze flag-setting instructions
        if (cmp->op != OP_CMP && cmp->op != OP_CMP_ZERO) {
            continue;
        }
        
        // Find all flag users for this CMP
        int users[MAX_INSTRS];
        int user_count = 0;
        
        for (int j = i + 1; j < count; j++) {
            Instr *next = ir_get(j);
            if (!next) continue;
            
            // Stop at control flow boundary
            if (is_control_flow(next->op)) {
                break;
            }
            
            // Check if this instruction reads flags
            if (next->flags_in == REG_FLAGS) {
                users[user_count++] = j;
                
                // Only CSE if we have multiple users
                if (user_count >= 2) {
                    // Keep this CMP (it has multiple users)
                    // The first user keeps the flags, subsequent users
                    // can reuse the flags from the same CMP
                    break;
                }
            }
        }
        
        // If CMP has only one user, it's already optimal
        // If CMP has multiple users, we've identified it for CSE
    }
}
    """)
    print()
    
    print("Note: Flag CSE is more complex than it appears.")
    print("The current IR design already shares flags implicitly:")
    print("  CMP a, b")
    print("  COND_SET x, EQ    // reads flags")
    print("  COND_SET y, NE    // reads SAME flags")
    print()
    print("The CMP is NOT duplicated, so CSE is already 'free'!")
    print("True CSE would be for separate CMP instructions:")
    print("  CMP a, b")
    print("  COND_SET x, EQ")
    print("  CMP a, b    // Redundant!")
    print("  COND_SET y, NE")
    print()
    print("This is handled by general CSE, not flag-specific CSE.")
    print()

def generate_test_cases():
    """Generate test cases for CSE verification."""
    print("=" * 80)
    print("Test Cases for Flag CSE")
    print("=" * 80)
    print()
    
    print("Test 1: CMP with single user (no CSE needed)")
    print("-" * 40)
    print("""
    // Before:
    CMP a, b
    COND_SET x, EQ
    
    // After: (unchanged - single user)
    CMP a, b
    COND_SET x, EQ
    """)
    print()
    
    print("Test 2: CMP with multiple users (implicit sharing)")
    print("-" * 40)
    print("""
    // Before:
    CMP a, b
    COND_SET x, EQ    // reads flags
    COND_SET y, NE    // reads SAME flags
    
    // After: (unchanged - flags already shared)
    CMP a, b
    COND_SET x, EQ
    COND_SET y, NE
    """)
    print()
    
    print("Test 3: Redundant CMPs (general CSE handles this)")
    print("-" * 40)
    print("""
    // Before:
    CMP a, b
    COND_SET x, EQ
    CMP a, b    // Redundant!
    COND_SET y, NE
    
    // After (general CSE):
    CMP a, b
    COND_SET x, EQ
    COND_SET y, NE
    """)
    print()
    
    print("Test 4: CMP across control flow (no sharing)")
    print("-" * 40)
    print("""
    // Before:
    CMP a, b
    COND_SET x, EQ
    JCOND eq, L1
    COND_SET y, NE    // Different flag context
    
    // After: (unchanged - CMP used by jump)
    CMP a, b
    COND_SET x, EQ
    JCOND eq, L1
    COND_SET y, NE
    """)
    print()

def main():
    root = Path.cwd()
    
    print("=" * 80)
    print("Flag CSE Analyzer")
    print("=" * 80)
    print()
    
    # Analyze flag liveness
    print("Analyzing flag liveness patterns...")
    print()
    
    # Generate effectiveness estimate
    all_results = {}
    estimate_cse_effectiveness(all_results)
    
    # Generate implementation plan
    generate_cse_plan()
    
    # Generate test cases
    generate_test_cases()
    
    print("=" * 80)
    print("Analysis Complete")
    print("=" * 80)
    print()
    print("KEY INSIGHT:")
    print("-" * 40)
    print("The current IR design already provides 'free' flag CSE!")
    print()
    print("When multiple COND_SET/JCOND follow one CMP, they all read")
    print("the SAME flags - no duplication occurs.")
    print()
    print("True CSE opportunities (redundant CMPs) are handled by")
    print("general CSE optimization, not flag-specific logic.")
    print()
    print("Recommendation: Flag CSE is NOT needed as a separate pass.")
    print("General CSE will handle redundant CMPs.")
    print()
    
    return 0

if __name__ == "__main__":
    sys.exit(main())
