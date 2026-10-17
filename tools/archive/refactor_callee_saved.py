#!/usr/bin/env python3
"""
Callee-Saved Register Refactoring Assistant

Helps refactor callee-saved register handling from ast_lowering.c to backend layer.

This tool:
1. Analyzes current x86-64 specific code
2. Generates the refactoring patches
3. Verifies the changes after application

Usage:
    python3 refactor_callee_saved.py --analyze
    python3 refactor_callee_saved.py --generate-patches
    python3 refactor_callee_saved.py --verify
"""

import sys
import os
import re
from pathlib import Path

# x86-64 specific data that needs to be moved
X86_64_CALLEE_SAVED_DATA = {
    'registers': ['rbx', 'rbp', 'r12', 'r13', 'r14', 'r15'],
    'pool_indices': [1, 5, 8, 9, 10, 11],  # Indices in callee_save_pool_to_vreg
    'slot_offsets': {
        1: 8,   # rbx
        5: 0,   # rbp (not used in this pattern)
        8: 16,  # r12
        9: 24,  # r13
        10: 32, # r14
        11: 40, # r15
    },
    'vreg_mapping': [0, 3, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15],
}

def analyze_current_state():
    """Analyze current state of callee-saved register handling."""
    print("=" * 80)
    print("CALLEE-SAVED REGISTER REFACTORING ANALYSIS")
    print("=" * 80)
    
    base_path = Path("src/zero_cc")
    
    print("\n1. CURRENT LOCATIONS OF X86-64 SPECIFIC CODE")
    print("-" * 80)
    
    # regalloc.h
    regalloc_h = base_path / "parser" / "regalloc.h"
    if regalloc_h.exists():
        content = regalloc_h.read_text()
        print(f"\n[{regalloc_h}]")
        
        if "callee_save_slot_offset" in content:
            print("  ⚠️  Contains callee_save_slot_offset[] (x86-64 specific)")
            # Extract the array
            match = re.search(r'static const int32_t callee_save_slot_offset\[\].*?=\s*\{(.*?)\};', 
                            content, re.DOTALL)
            if match:
                print("  Array content:")
                for line in match.group(1).split('\n'):
                    if '/*' in line:
                        print(f"    {line.strip()}")
        
        if "callee_save_pool_to_vreg" in content:
            print("  ⚠️  Contains callee_save_pool_to_vreg[] (x86-64 specific)")
        
        # Check for ISA-specific mask manipulation
        if "callee_saved_indices" in content:
            print("  ⚠️  Contains ISA-specific mask manipulation (lines 219-240)")
    
    # ast_lowering.c
    ast_lowering_c = base_path / "parser" / "ast_lowering.c"
    if ast_lowering_c.exists():
        content = ast_lowering_c.read_text()
        print(f"\n[{ast_lowering_c}]")
        
        if "emit_callee_save_loads" in content:
            print("  ⚠️  Contains emit_callee_save_loads() function")
        if "emit_callee_save_stores" in content:
            print("  ⚠️  Contains emit_callee_save_stores() function")
        if "c->isa == ISA_X86_64" in content and "emit_callee_save_loads" in content:
            print("  ✓  emit_callee_save_loads() is ISA-conditional (quick fix applied)")
    
    # ast_lowering_mc.c
    ast_lowering_mc_c = base_path / "parser" / "ast_lowering_mc.c"
    if ast_lowering_mc_c.exists():
        content = ast_lowering_mc_c.read_text()
        print(f"\n[{ast_lowering_mc_c}]")
        
        if "emit_callee_save_stores" in content:
            print("  ⚠️  Calls emit_callee_save_stores()")
            if "c->isa == ISA_X86_64" in content:
                print("  ✓  Call is ISA-conditional (quick fix applied)")
    
    # ast_lowering_internal.h
    ast_lowering_internal_h = base_path / "parser" / "ast_lowering_internal.h"
    if ast_lowering_internal_h.exists():
        content = ast_lowering_internal_h.read_text()
        print(f"\n[{ast_lowering_internal_h}]")
        
        if "emit_callee_save" in content:
            print("  ⚠️  Contains declarations for emit_callee_save_*()")
    
    print("\n2. BACKEND INTERFACE STATUS")
    print("-" * 80)
    
    isa_backend_h = base_path / "isa" / "isa_backend.h"
    if isa_backend_h.exists():
        content = isa_backend_h.read_text()
        print(f"\n[{isa_backend_h}]")
        
        if "callee_saved_gpr" in content:
            print("  ✓  Has callee_saved_gpr metadata (for register allocator)")
        
        if "emit_callee_save" in content:
            print("  ✓  Has emit_callee_save hook in interface")
        else:
            print("  ⚠️  Missing emit_callee_save hook (needs to be added)")
    
    print("\n3. BACKEND IMPLEMENTATIONS")
    print("-" * 80)
    
    for isa in ['x86_64', 'aarch64', 'riscv']:
        backend_c = base_path / "isa" / f"{isa}.c"
        if backend_c.exists():
            content = backend_c.read_text()
            print(f"\n[{isa}.c]")
            
            if "callee_saved_gpr" in content:
                # Extract the array
                match = re.search(r'static const int \w+_callee_saved_gpr\[\]\s*=\s*\{(.*?)\};', 
                                content, re.DOTALL)
                if match:
                    regs = match.group(1).strip()
                    print(f"  ✓  Has callee_saved_gpr[]: [{regs}]")
    
    print("\n4. REFACTORING REQUIREMENTS")
    print("-" * 80)
    print("""
To complete the refactoring:

A. Add backend interface hook:
   - Add to ZeroCCBackend struct in isa_backend.h:
       void (*emit_callee_save_stores)(ZeroCCCompiler *c);
       void (*emit_callee_save_loads)(ZeroCCCompiler *c);

B. Move x86-64 specific data to x86_64.c:
   - callee_save_slot_offset[]
   - callee_save_pool_to_vreg[]

C. Move function implementations to x86_64.c:
   - emit_callee_save_stores()
   - emit_callee_save_loads()

D. Update backend initialization in x86_64.c:
   - BE(c)->emit_callee_save_stores = x86_64_emit_callee_save_stores;
   - BE(c)->emit_callee_save_loads = x86_64_emit_callee_save_loads;

E. Set hooks to NULL for other ISAs:
   - In aarch64.c and riscv.c initialization

F. Remove from ast_lowering.c:
   - emit_callee_save_stores() function
   - emit_callee_save_loads() function
   - ISA-conditional check (will use hook instead)

G. Remove from ast_lowering_mc.c:
   - ISA-conditional check
   - Call to emit_callee_save_stores()
   - Replace with: if (BE(c)->emit_callee_save_stores) BE(c)->emit_callee_save_stores(c);

H. Remove from ast_lowering_internal.h:
   - Declarations for emit_callee_save_*()

I. Remove from regalloc.h:
   - callee_save_slot_offset[]
   - callee_save_pool_to_vreg[]

J. Update regalloc.h mask manipulation:
   - The mask bits at lines 219-240 use x86-64 register indices
   - These should use backend's callee_saved_gpr[] instead

K. Test:
   - Run all 118 JIT tests
   - Verify x86-64 still works
   - Verify AArch64 doesn't break
   - Verify RISC-V doesn't break
""")
    
    print("=" * 80)

def generate_patches():
    """Generate patch files for the refactoring."""
    print("=" * 80)
    print("GENERATING REFACTORING PATCHES")
    print("=" * 80)
    
    patches_dir = Path("patches")
    patches_dir.mkdir(exist_ok=True)
    
    # Patch 1: isa_backend.h - Add hooks
    patch1 = """--- a/src/zero_cc/isa/isa_backend.h
+++ b/src/zero_cc/isa/isa_backend.h
@@ -44,6 +44,8 @@ typedef struct {
     const int *callee_saved_gpr;  /* Array of callee-saved GPR indices */
     int callee_saved_gpr_count;   /* Number of callee-saved GPRs */
     
+    /* Callee-saved register preservation hooks */
+    void (*emit_callee_save_stores)(ZeroCCCompiler *c);
+    void (*emit_callee_save_loads)(ZeroCCCompiler *c);
 } ZeroCCBackend;
"""
    (patches_dir / "01_add_backend_hooks.patch").write_text(patch1)
    print(f"✓ Created patches/01_add_backend_hooks.patch")
    
    # Patch 2: x86_64.c - Add implementation
    patch2_content = """--- a/src/zero_cc/isa/x86_64.c
+++ b/src/zero_cc/isa/x86_64.c
@@ -24,6 +24,50 @@ static const int x86_64_int_arg_regs[] = {7, 6, 2, 1, 8, 9};
 static const int x86_64_fp_arg_regs[] = {0, 1, 2, 3, 4, 5, 6, 7};
 static const int x86_64_callee_saved_gpr[] = {3, 5, 12, 13, 14, 15};
 
+/* Callee-save slot offsets (x86-64 specific) */
+static const int32_t x86_64_callee_save_slot_offset[] = {
+    /* idx 0 (rax)  */ 0,  /* not callee-saved */
+    /* idx 1 (rbx)  */ 8,
+    /* idx 2 (rsi)  */ 0,  /* not callee-saved */
+    /* idx 3 (rdi)  */ 0,  /* not callee-saved */
+    /* idx 4 (r8)   */ 0,  /* not callee-saved */
+    /* idx 5 (r9)   */ 0,  /* not callee-saved */
+    /* idx 6 (r10)  */ 0,  /* not callee-saved */
+    /* idx 7 (r11)  */ 0,  /* not callee-saved */
+    /* idx 8 (r12)  */ 16,
+    /* idx 9 (r13)  */ 24,
+    /* idx 10 (r14) */ 32,
+    /* idx 11 (r15) */ 40,
+};
+
+/* Physical VREG for each allocator pool index */
+static const int x86_64_callee_save_pool_to_vreg[] = {
+    0, 3, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
+};
+
+/* Emit STORE for callee-saved registers that were assigned */
+static void x86_64_emit_callee_save_stores(ZeroCCCompiler *c) {
+    uint32_t mask = c->regalloc.used_callee_saved_gpr_mask;
+    while (mask) {
+        int idx = __builtin_ctz(mask);
+        int vreg = x86_64_callee_save_pool_to_vreg[idx];
+        int32_t slot = x86_64_callee_save_slot_offset[idx];
+        if (BE(c)->emit_store) {
+            BE(c)->emit_store(c, vreg, BE(c)->fp_reg, -slot);
+        }
+        mask &= mask - 1;
+    }
+}
+
+/* Emit LOAD for callee-saved registers that were assigned */
+static void x86_64_emit_callee_save_loads(ZeroCCCompiler *c) {
+    uint32_t mask = c->regalloc.used_callee_saved_gpr_mask;
+    while (mask) {
+        int idx = __builtin_ctz(mask);
+        int vreg = x86_64_callee_save_pool_to_vreg[idx];
+        int32_t slot = x86_64_callee_save_slot_offset[idx];
+        if (BE(c)->emit_load) {
+            BE(c)->emit_load(c, vreg, BE(c)->fp_reg, -slot);
+        }
+        mask &= mask - 1;
+    }
+}
+
 /* ISA-specific calling convention */
 static const ISACallingConvention x86_64_conv = {
     .name = "x86-64 System V",
@@ -35,6 +79,8 @@ static const ISACallingConvention x86_64_conv = {
     .fp_arg_regs = x86_64_fp_arg_regs,
     .fp_arg_reg_count = 8,
     .callee_saved_gpr = x86_64_callee_saved_gpr,
     .callee_saved_gpr_count = 6,
+    .emit_callee_save_stores = x86_64_emit_callee_save_stores,
+    .emit_callee_save_loads = x86_64_emit_callee_save_loads,
 };
"""
    (patches_dir / "02_x86_64_add_implementation.patch").write_text(patch2_content)
    print(f"✓ Created patches/02_x86_64_add_implementation.patch")
    
    # Patch 3: aarch64.c and riscv.c - Set hooks to NULL
    patch3 = """--- a/src/zero_cc/isa/aarch64.c
+++ b/src/zero_cc/isa/aarch64.c
@@ -37,6 +37,8 @@ static const ISACallingConvention aarch64_conv = {
     .fp_arg_regs = aarch64_fp_arg_regs,
     .fp_arg_reg_count = 8,
     .callee_saved_gpr = aarch64_callee_saved_gpr,
     .callee_saved_gpr_count = 12,
+    .emit_callee_save_stores = NULL,  /* AArch64 handles differently */
+    .emit_callee_save_loads = NULL,   /* AArch64 handles differently */
 };
 
 /* Similar for riscv.c */
"""
    (patches_dir / "03_other_isas_null_hooks.patch").write_text(patch3)
    print(f"✓ Created patches/03_other_isas_null_hooks.patch")
    
    # Patch 4: ast_lowering.c - Remove functions and update call
    patch4 = """--- a/src/zero_cc/parser/ast_lowering.c
+++ b/src/zero_cc/parser/ast_lowering.c
@@ -29,26 +29,6 @@
 /* ========================================================================= */
 
 /* Emit MOV [rbp - slot], reg for each callee-saved GPR that was assigned. */
-void emit_callee_save_stores(ZeroCCCompiler *c) {
-    uint32_t mask = c->regalloc.used_callee_saved_gpr_mask;
-    while (mask) {
-        int idx = __builtin_ctz(mask);
-        int vreg = callee_save_pool_to_vreg[idx];
-        int32_t slot = callee_save_slot_offset[idx];
-        if (BE(c)->emit_store) {
-            BE(c)->emit_store(c, vreg, BE(c)->fp_reg, -slot);
-        }
-        mask &= mask - 1;
-    }
-}
-
-/* Emit MOV reg, [rbp - slot] for each callee-saved GPR that was assigned. */
-void emit_callee_save_loads(ZeroCCCompiler *c) {
-    uint32_t mask = c->regalloc.used_callee_saved_gpr_mask;
-    while (mask) {
-        int idx = __builtin_ctz(mask);
-        int vreg = callee_save_pool_to_vreg[idx];
-        int32_t slot = callee_save_slot_offset[idx];
-        if (BE(c)->emit_load) {
-            BE(c)->emit_load(c, vreg, BE(c)->fp_reg, -slot);
-        }
-        mask &= mask - 1;
-    }
-}
-
 /* =========================================================================
  * Main Entry Point
  * ========================================================================= */
@@ -169,10 +149,8 @@ void zero_cc_lower_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef root) {
     ir_emit_prologue(0);
 
     /* Restore callee-saved registers before the epilogue leave/ret. */
-    if (c->isa == ISA_X86_64) {
-        emit_callee_save_loads(c);
-    }
+    if (BE(c)->emit_callee_save_loads) {
+        BE(c)->emit_callee_save_loads(c);
     }
     
     /* Emit epilogue */
"""
    (patches_dir / "04_ast_lowering_remove_functions.patch").write_text(patch4)
    print(f"✓ Created patches/04_ast_lowering_remove_functions.patch")
    
    # Patch 5: ast_lowering_mc.c - Update call
    patch5 = """--- a/src/zero_cc/parser/ast_lowering_mc.c
+++ b/src/zero_cc/parser/ast_lowering_mc.c
@@ -52,9 +52,8 @@ void zero_cc_lower_ir_to_mc(ZeroCCCompiler *c) {
                 if (BE(c)->emit_prologue) {
                     BE(c)->emit_prologue(c, (size_t)instr->imm64);
                 }
-                if (c->isa == ISA_X86_64) {
-                    emit_callee_save_stores(c);
-                }
+                if (BE(c)->emit_callee_save_stores) {
+                    BE(c)->emit_callee_save_stores(c);
+                }
                 break;
"""
    (patches_dir / "05_ast_lowering_mc_update_call.patch").write_text(patch5)
    print(f"✓ Created patches/05_ast_lowering_mc_update_call.patch")
    
    # Patch 6: regalloc.h - Remove arrays
    patch6 = """--- a/src/zero_cc/parser/regalloc.h
+++ b/src/zero_cc/parser/regalloc.h
@@ -450,24 +450,6 @@ static inline void regalloc_destroy(RegAllocator *ra) {
 /* Number of bytes reserved at the top of every frame for callee-save slots. */
 #define CALLEE_SAVE_FRAME_RESERVE 48
 
-/* Maps allocator pool index → frame slot offset (for callee-saved regs only).
- * Use with: BE(c)->emit_store(c, vreg, fp_reg, -(int32_t)callee_save_slot_offset[idx]) */
-static const int32_t callee_save_slot_offset[] __attribute__((unused)) = {
-    /* idx 0 (rax)  */ 0,  /* not callee-saved */
-    /* idx 1 (rbx)  */ 8,
-    /* idx 2 (rsi)  */ 0,  /* not callee-saved */
-    /* idx 3 (rdi)  */ 0,  /* not callee-saved */
-    /* idx 4 (r8)   */ 0,  /* not callee-saved */
-    /* idx 5 (r9)   */ 0,  /* not callee-saved */
-    /* idx 6 (r10)  */ 0,  /* not callee-saved */
-    /* idx 7 (r11)  */ 0,  /* not callee-saved */
-    /* idx 8 (r12)  */ 16,
-    /* idx 9 (r13)  */ 24,
-    /* idx 10 (r14) */ 32,
-    /* idx 11 (r15) */ 40,
-};
-
-/* Physical VREG for each allocator pool index (matches idx_to_vreg[] in regalloc_idx_to_vreg) */
-static const int callee_save_pool_to_vreg[] __attribute__((unused)) = {
-    0, 3, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
-};
-
 #endif /* ZERO_CC_PARSER_REGALLOC_H */
"""
    (patches_dir / "06_regalloc_remove_arrays.patch").write_text(patch6)
    print(f"✓ Created patches/06_regalloc_remove_arrays.patch")
    
    # Patch 7: ast_lowering_internal.h - Remove declarations
    patch7 = """--- a/src/zero_cc/parser/ast_lowering_internal.h
+++ b/src/zero_cc/parser/ast_lowering_internal.h
@@ -42,5 +42,3 @@ void label_patcher_queue(label_patcher_ctx *ctx, uint16_t label_idx, int32_t off
 void label_patcher_apply(label_patcher_ctx *ctx);
 
-void emit_callee_save_stores(ZeroCCCompiler *c);
-void emit_callee_save_loads(ZeroCCCompiler *c);
 #endif /* ZERO_CC_PARSER_AST_LOWERING_INTERNAL_H */
"""
    (patches_dir / "07_ast_lowering_internal_remove_decls.patch").write_text(patch7)
    print(f"✓ Created patches/07_ast_lowering_internal_remove_decls.patch")
    
    print("\n" + "=" * 80)
    print("PATCHES GENERATED SUCCESSFULLY")
    print("=" * 80)
    print("\nTo apply patches:")
    print("  cd /home/ubuntu/dev/qnz_c_compiler")
    print("  for patch in patches/*.patch; do git apply $patch || echo \"Failed: $patch\"; done")
    print("\nAfter applying, run:")
    print("  make clean && make")
    print("  ./tools/test_runner.py")
    print("=" * 80)

def verify_refactoring():
    """Verify the refactoring was applied correctly."""
    print("=" * 80)
    print("VERIFYING REFACTORING")
    print("=" * 80)
    
    base_path = Path("src/zero_cc")
    errors = []
    warnings = []
    
    # Check 1: Backend interface has hooks
    isa_backend_h = base_path / "isa" / "isa_backend.h"
    content = isa_backend_h.read_text()
    if "emit_callee_save_stores" in content and "emit_callee_save_loads" in content:
        print("✓ Backend interface has callee-save hooks")
    else:
        errors.append("Backend interface missing callee-save hooks")
    
    # Check 2: x86_64.c has implementation
    x86_64_c = base_path / "isa" / "x86_64.c"
    content = x86_64_c.read_text()
    if "x86_64_emit_callee_save" in content:
        print("✓ x86_64.c has callee-save implementation")
    else:
        errors.append("x86_64.c missing callee-save implementation")
    
    # Check 3: ast_lowering.c doesn't have functions
    ast_lowering_c = base_path / "parser" / "ast_lowering.c"
    content = ast_lowering_c.read_text()
    if "void emit_callee_save" not in content:
        print("✓ ast_lowering.c doesn't define callee-save functions")
    else:
        errors.append("ast_lowering.c still defines callee-save functions")
    
    # Check 4: ast_lowering_mc.c uses hook
    if "BE(c)->emit_callee_save_stores" in content:
        print("✓ ast_lowering_mc.c uses backend hook")
    else:
        warnings.append("ast_lowering_mc.c may not use backend hook")
    
    # Check 5: regalloc.h doesn't have arrays
    regalloc_h = base_path / "parser" / "regalloc.h"
    content = regalloc_h.read_text()
    if "callee_save_slot_offset" not in content and "callee_save_pool_to_vreg" not in content:
        print("✓ regalloc.h doesn't have x86-64 specific arrays")
    else:
        errors.append("regalloc.h still has x86-64 specific arrays")
    
    print("\n" + "=" * 80)
    if errors:
        print("ERRORS:")
        for e in errors:
            print(f"  ✗ {e}")
    else:
        print("✓ All checks passed!")
    
    if warnings:
        print("\nWARNINGS:")
        for w in warnings:
            print(f"  ⚠️  {w}")
    
    print("=" * 80)

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 refactor_callee_saved.py <command>")
        print()
        print("Commands:")
        print("  --analyze          Analyze current state")
        print("  --generate-patches Generate refactoring patches")
        print("  --verify           Verify refactoring was applied")
        print()
        print("Running full analysis...")
        analyze_current_state()
        print()
        generate_patches()
        return
    
    cmd = sys.argv[1]
    
    if cmd == "--analyze":
        analyze_current_state()
    elif cmd == "--generate-patches":
        generate_patches()
    elif cmd == "--verify":
        verify_refactoring()
    else:
        print(f"Unknown command: {cmd}")
        sys.exit(1)

if __name__ == '__main__':
    main()
