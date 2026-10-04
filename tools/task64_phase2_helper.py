#!/usr/bin/env python3
"""Task 64 Phase 2 Helper - Calling Convention Abstraction Assistant

This tool helps identify and plan the abstraction of calling convention
tables from the IR layer to the ISA backend.
"""

import re
import os

def find_calling_convention_tables():
    """Find calling convention tables in the codebase"""
    patterns = {
        'int_arg_regs': r'int_arg_regs\[(\d+)\]\s*=\s*\{([^}]+)\}',
        'fp_arg_regs': r'fp_arg_regs\[(\d+)\]\s*=\s*\{([^}]+)\}',
        'callee_saved': r'callee_saved\[(\d+)\]',
        'arg_reg_names': r'int_arg_regs\[8\]\s*=\s*\{([^}]+)\}',
    }
    
    files_to_check = [
        'src/zero_cc/parser/ast_lowering_mc.c',
        'src/zero_cc/parser/regalloc.h',
        'src/zero_cc/isa/x86_64.c',
        'src/zero_cc/isa/aarch64.c',
        'src/zero_cc/isa/riscv.c',
    ]
    
    results = {}
    
    for filepath in files_to_check:
        if not os.path.exists(filepath):
            continue
            
        with open(filepath, 'r') as f:
            content = f.read()
            lines = content.split('\n')
        
        results[filepath] = []
        
        for i, line in enumerate(lines, 1):
            for pattern_name, pattern in patterns.items():
                if re.search(pattern, line):
                    match = re.search(pattern, line)
                    results[filepath].append({
                        'line': i,
                        'pattern': pattern_name,
                        'content': line.strip()[:100]
                    })
    
    return results

def generate_backend_abstraction():
    """Generate the proposed backend abstraction code"""
    return '''
// Add to isa_backend.h after the ISARegInfo struct:

/* Calling convention tables - ISA-specific */
typedef struct {
    const int *int_arg_regs;      /* Array of int arg register indices */
    int int_arg_reg_count;        /* Number of int arg registers */
    const int *fp_arg_regs;       /* Array of FP arg register indices */
    int fp_arg_reg_count;         /* Number of FP arg registers */
    const int *callee_saved_gpr;  /* Array of callee-saved GPR indices */
    int callee_saved_gpr_count;   /* Number of callee-saved GPRs */
} ISACallingConvention;

/* Add to ZeroCCISABackend struct: */
const ISACallingConvention *conv;  /* Calling convention tables */

/* Helper functions to add: */
static inline int backend_get_int_arg_reg(const ZeroCCISABackend *be, int idx) {
    if (be->conv && idx < be->conv->int_arg_reg_count) {
        return be->conv->int_arg_regs[idx];
    }
    return -1;  /* Stack-passed */
}

static inline int backend_get_fp_arg_reg(const ZeroCCISABackend *be, int idx) {
    if (be->conv && idx < be->conv->fp_arg_reg_count) {
        return be->conv->fp_arg_regs[idx];
    }
    return -1;  /* Stack-passed */
}
'''

def generate_x86_64_conv():
    """Generate x86-64 calling convention tables"""
    return '''
// Add to x86_64.c:

/* x86-64 System V ABI calling convention */
static const int x86_64_int_arg_regs[] = {7, 6, 2, 1, 8, 9};  /* rdi, rsi, rdx, rcx, r8, r9 */
static const int x86_64_fp_arg_regs[] = {13, 14, 15, 16, 17, 18, 19, 20};  /* xmm0-xmm7 */
static const int x86_64_callee_saved_gpr[] = {3, 4, 10, 11, 12, 13, 14};  /* rbx, rbp, r12-r15 */

static const ISACallingConvention x86_64_conv = {
    .int_arg_regs = x86_64_int_arg_regs,
    .int_arg_reg_count = 6,
    .fp_arg_regs = x86_64_fp_arg_regs,
    .fp_arg_reg_count = 8,
    .callee_saved_gpr = x86_64_callee_saved_gpr,
    .callee_saved_gpr_count = 7,
};

// Update x86_64_backend initialization:
const ZeroCCISABackend x86_64_backend = {
    .isa = ISA_X86_64,
    .regs = &x86_64_regs,
    .conv = &x86_64_conv,  // Add this line
    // ... rest of initialization
};
'''

def generate_migration_guide():
    """Generate migration guide for ast_lowering_mc.c"""
    return '''
// Migration steps for ast_lowering_mc.c:

// 1. Replace hardcoded int_arg_regs table:
// OLD:
//   static const int int_arg_regs[6] = {7, 6, 2, 1, 8, 9};
//   int dst_reg = int_arg_regs[a];

// NEW:
//   int dst_reg = backend_get_int_arg_reg(BE(c), a);
//   if (dst_reg < 0) {
//       /* Argument is stack-passed, handle accordingly */
//   }

// 2. Replace hardcoded register numbers with backend helpers:
// OLD:
//   BE(c)->emit_load(c, 10 /* r10 */, BE(c)->fp_reg, -slot_off);

// NEW:
//   int scratch_reg = 10;  /* Keep as-is for scratch registers */
//   BE(c)->emit_load(c, scratch_reg, BE(c)->fp_reg, -slot_off);

// 3. Use backend for callee-saved register iteration:
// OLD:
//   uint32_t mask = c->regalloc.used_callee_saved_gpr_mask;
//   while (mask) {
//       int idx = __builtin_ctz(mask);
//       int vreg = callee_save_pool_to_vreg[idx];

// NEW:
//   for (int i = 0; i < BE(c)->conv->callee_saved_gpr_count; i++) {
//       if (regalloc_is_callee_saved_used(&c->regalloc, i)) {
//           int vreg = callee_save_pool_to_vreg[i];
//           int phys_reg = BE(c)->conv->callee_saved_gpr[i];
//       }
//   }
'''

def main():
    print("=" * 70)
    print("Task 64 Phase 2 Helper - Calling Convention Abstraction")
    print("=" * 70)
    
    print("\n1. CALLING CONVENTION TABLES FOUND:")
    print("-" * 70)
    results = find_calling_convention_tables()
    
    for filepath, matches in results.items():
        if matches:
            print(f"\n{filepath}:")
            for match in matches:
                print(f"  Line {match['line']}: [{match['pattern']}]")
                print(f"    {match['content']}")
    
    print("\n2. PROPOSED BACKEND ABSTRACTION:")
    print("-" * 70)
    print(generate_backend_abstraction())
    
    print("\n3. X86-64 IMPLEMENTATION:")
    print("-" * 70)
    print(generate_x86_64_conv())
    
    print("\n4. MIGRATION GUIDE FOR ast_lowering_mc.c:")
    print("-" * 70)
    print(generate_migration_guide())
    
    print("\n" + "=" * 70)
    print("NEXT STEPS:")
    print("=" * 70)
    print("""
1. Add ISACallingConvention struct to isa_backend.h
2. Add conv field to ZeroCCISABackend struct
3. Add backend_get_int_arg_reg() and backend_get_fp_arg_reg() helpers
4. Implement calling convention tables for each ISA:
   - x86_64.c: x86_64_conv (System V ABI)
   - aarch64.c: aarch64_conv (AAPCS64)
   - riscv.c: riscv64_conv and riscv32_conv (ILP32/LP64D)
5. Update ast_lowering_mc.c to use backend_get_*_arg_reg() helpers
6. Verify all 118 tests still pass
""")

if __name__ == '__main__':
    main()
