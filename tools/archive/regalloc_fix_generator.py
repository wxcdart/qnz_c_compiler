#!/usr/bin/env python3
"""
Register Allocator Fix Generator

Generates the actual C code patches needed to fix caller-saved register clobbering.
Analyzes the register allocator and generates precise edits for:
1. src/zero_cc/parser/regalloc.h - Modify alloc_reg() to prefer callee-saved
2. src/zero_cc/parser/ast_lowering_mc.c - Mark intervals as spanning calls

Usage:
    python3 tools/regalloc_fix_generator.py --generate-patch
    python3 tools/regalloc_fix_generator.py --show-regalloc-code
    python3 tools/regalloc_fix_generator.py --verify-fix
"""

import argparse
import subprocess
import sys
import os
from pathlib import Path

class RegallocFixGenerator:
    """Generates fix code for caller-saved register handling"""
    
    def __init__(self, repo_root: str = "/home/ubuntu/dev/qnz_c_compiler"):
        self.repo_root = Path(repo_root)
        self.regalloc_header = self.repo_root / "src/zero_cc/parser/regalloc.h"
        self.lowering_file = self.repo_root / "src/zero_cc/parser/ast_lowering_mc.c"
    
    def show_regalloc_code(self):
        """Show relevant parts of regalloc.h"""
        if not self.regalloc_header.exists():
            print(f"Error: {self.regalloc_header} not found")
            sys.exit(1)
        
        content = self.regalloc_header.read_text()
        
        # Find alloc_reg function
        lines = content.split('\n')
        
        print("=" * 80)
        print("CURRENT alloc_reg() FUNCTION")
        print("=" * 80)
        print("")
        
        in_func = False
        brace_count = 0
        func_lines = []
        
        for i, line in enumerate(lines):
            if 'alloc_reg' in line and 'static' in line:
                in_func = True
            
            if in_func:
                func_lines.append(f"{i+1:4d}: {line}")
                brace_count += line.count('{') - line.count('}')
                
                if brace_count == 0 and '{' in ''.join(lines[:i+1]):
                    break
        
        print('\n'.join(func_lines[:50]))  # Limit output
        print("")
    
    def generate_patch_for_regalloc(self) -> str:
        """Generate patch for regalloc.h"""
        patch = """
/* ============================================================================
 * FIX: Prefer callee-saved registers for intervals spanning calls
 * ============================================================================
 * 
 * This patch modifies alloc_reg() to prefer callee-saved registers (x19-x28)
 * for intervals that span function calls. This prevents caller-saved register
 * clobbering issues.
 * 
 * Location: src/zero_cc/parser/regalloc.h
 * 
 */

/* Find this section in regalloc.h (around line 200-250): */

/* Get a free physical register for a virtual register (interval) */
static int alloc_reg(RegAllocator *ra, LiveInterval *interval) {
    if (!ra || !interval) return -1;
    
    /* Try to find a free register */
    for (int i = 0; i < NUM_PHYS_REGS; i++) {
        if (ra->reg_free[i]) {
            ra->reg_free[i] = false;
            return i;
        }
    }
    
    /* No free register - need to spill */
    return -1;
}

/* Replace with this version: */

/* Get a free physical register for a virtual register (interval) */
static int alloc_reg(RegAllocator *ra, LiveInterval *interval) {
    if (!ra || !interval) return -1;
    
    /* 
     * FIX: Prefer callee-saved registers for intervals spanning calls
     * AArch64: x19-x28 are callee-saved, x0-x18 are caller-saved
     */
    if (interval->spans_call) {
        /* Try callee-saved registers first (x19-x28) */
        for (int i = 19; i <= 28; i++) {
            if (ra->reg_free[i]) {
                ra->reg_free[i] = false;
                return i;
            }
        }
    }
    
    /* Try caller-saved registers (x0-x18) */
    for (int i = 0; i <= 18; i++) {
        if (ra->reg_free[i]) {
            ra->reg_free[i] = false;
            return i;
        }
    }
    
    /* No free register - need to spill */
    return -1;
}

/* ============================================================================
 * END FIX
 * ============================================================================
 */
"""
        return patch
    
    def generate_patch_for_lowering(self) -> str:
        """Generate patch for ast_lowering_mc.c"""
        patch = """
/* ============================================================================
 * FIX: Mark intervals as spanning calls before register allocation
 * ============================================================================
 * 
 * This patch adds code to mark LiveInterval.spans_call = true for any
 * interval that spans a function call instruction. This must be done
 * BEFORE register allocation begins.
 * 
 * Location: src/zero_cc/parser/ast_lowering_mc.c
 * 
 */

/* Add this structure to track call indices: */

typedef struct {
    size_t *call_instruction_indices;  /* IR indices of CALL instructions */
    size_t call_count;                  /* Number of calls */
    size_t call_capacity;               /* Allocated capacity */
} CallTracking;

/* In lower_ir_to_mc() function, before calling regalloc_allocate_registers(): */

/* Step 1: Track call instruction indices */
CallTracking calls = {0};
calls.call_capacity = 64;
calls.call_instruction_indices = malloc(calls.call_capacity * sizeof(size_t));
calls.call_count = 0;

for (size_t i = 0; i < instr_count; i++) {
    if (instrs[i].op == OP_CALL) {
        if (calls.call_count >= calls.call_capacity) {
            calls.call_capacity *= 2;
            calls.call_instruction_indices = realloc(
                calls.call_instruction_indices, 
                calls.call_capacity * sizeof(size_t)
            );
        }
        calls.call_instruction_indices[calls.call_count++] = i;
    }
}

/* Step 2: Mark intervals that span calls */
for (size_t i = 0; i < c->regalloc.interval_count; i++) {
    LiveInterval *interval = &c->regalloc.intervals[i];
    
    for (size_t j = 0; j < calls.call_count; j++) {
        size_t call_idx = calls.call_instruction_indices[j];
        
        /* Check if interval spans this call */
        if (interval->start <= call_idx && interval->end >= call_idx) {
            interval->spans_call = true;
            break;
        }
    }
}

/* Step 3: Now call register allocation (it will prefer callee-saved) */
regalloc_allocate_registers(&c->regalloc);

/* Step 4: Clean up */
free(calls.call_instruction_indices);

/* ============================================================================
 * END FIX
 * ============================================================================
 */
"""
        return patch
    
    def generate_complete_patch(self) -> str:
        """Generate complete patch with both changes"""
        lines = []
        lines.append("=" * 80)
        lines.append("COMPLETE PATCH FOR CALLER-SAVED REGISTER FIX")
        lines.append("=" * 80)
        lines.append("")
        lines.append("This patch fixes the caller-saved register clobbering bug by:")
        lines.append("1. Marking intervals that span function calls")
        lines.append("2. Making register allocator prefer callee-saved registers for those intervals")
        lines.append("")
        lines.append("FILES TO MODIFY:")
        lines.append("-" * 80)
        lines.append("")
        lines.append("1. src/zero_cc/parser/regalloc.h")
        lines.append("   - Modify alloc_reg() function")
        lines.append("")
        lines.append("2. src/zero_cc/parser/ast_lowering_mc.c")
        lines.append("   - Add call tracking before register allocation")
        lines.append("   - Mark intervals as spanning calls")
        lines.append("")
        lines.append("TESTING:")
        lines.append("-" * 80)
        lines.append("")
        lines.append("  make clean && make")
        lines.append("  make test  # All 118 tests should pass")
        lines.append("  ./build/bin/zerocc -c test_accum.c -o test.o --target aarch64")
        lines.append("  qemu-aarch64-static test  # Should return correct value")
        lines.append("")
        lines.append("VERIFICATION:")
        lines.append("-" * 80)
        lines.append("")
        lines.append("  python3 tools/register_clobbering_analyzer.py --object test.o")
        lines.append("  # Should show: No register clobbering issues detected!")
        lines.append("")
        lines.append("=" * 80)
        lines.append("")
        lines.append(self.generate_patch_for_regalloc())
        lines.append("")
        lines.append(self.generate_patch_for_lowering())
        
        return '\n'.join(lines)
    
    def verify_fix_needed(self) -> bool:
        """Check if fix is needed by analyzing current code"""
        if not self.regalloc_header.exists():
            return False
        
        content = self.regalloc_header.read_text()
        
        # Check if alloc_reg already handles spans_call
        if 'spans_call' in content and 'callee' in content.lower():
            print("✓ Fix appears to already be implemented")
            return False
        
        print("⚠️  Caller-saved register fix is needed")
        print("")
        print("Current alloc_reg() does NOT handle spans_call intervals")
        print("This means caller-saved registers will be assigned to temps spanning calls")
        print("Result: Register clobbering bugs!")
        print("")
        
        return True

def main():
    parser = argparse.ArgumentParser(
        description='Generate fix code for caller-saved register handling'
    )
    parser.add_argument('--generate-patch', action='store_true', 
                       help='Generate complete patch')
    parser.add_argument('--show-regalloc-code', action='store_true',
                       help='Show current alloc_reg() code')
    parser.add_argument('--verify-fix', action='store_true',
                       help='Verify if fix is needed')
    parser.add_argument('--output', help='Output file for patch')
    
    args = parser.parse_args()
    
    generator = RegallocFixGenerator()
    
    if args.verify_fix:
        needed = generator.verify_fix_needed()
        sys.exit(0 if needed else 1)
    
    if args.show_regalloc_code:
        generator.show_regalloc_code()
    
    if args.generate_patch:
        patch = generator.generate_complete_patch()
        
        if args.output:
            with open(args.output, 'w') as f:
                f.write(patch)
            print(f"Patch saved to: {args.output}")
        else:
            print(patch)
    
    sys.exit(0)

if __name__ == '__main__':
    main()
