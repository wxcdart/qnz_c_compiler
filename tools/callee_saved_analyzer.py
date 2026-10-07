#!/usr/bin/env python3
"""
Callee-Saved Register Analyzer

Analyzes callee-saved register handling across ISAs and verifies
the refactoring from ast_lowering.c to backend layer.

Usage:
    python3 callee_saved_analyzer.py --check-isas
    python3 callee_saved_analyzer.py --verify-backend
    python3 callee_saved_analyzer.py --generate-report
"""

import sys
import os
import re
from pathlib import Path

# Callee-saved registers per ISA
CALLEE_SAVED_REGS = {
    'x86_64': {
        'registers': ['rbx', 'r12', 'r13', 'r14', 'r15'],
        'slots': {'rbx': 8, 'r12': 16, 'r13': 24, 'r14': 32, 'r15': 40},
        'calling_convention': 'System V AMD64 ABI'
    },
    'aarch64': {
        'registers': ['x19', 'x20', 'x21', 'x22', 'x23', 'x24', 'x25', 'x26', 'x27', 'x28', 'lr'],
        'slots': {},  # AArch64 handles differently (often in prologue)
        'calling_convention': 'AAPCS64'
    },
    'riscv64': {
        'registers': ['s0-fp', 's1', 's2-s11', 'ra'],
        'slots': {},  # RISC-V handles differently
        'calling_convention': 'LP64D'
    }
}

def check_isa_implementation():
    """Check current ISA-specific implementations."""
    print("=" * 70)
    print("CALLEE-SAVED REGISTER IMPLEMENTATION STATUS")
    print("=" * 70)
    
    base_path = Path("src/zero_cc")
    
    # Check ast_lowering.c
    ast_lowering_path = base_path / "parser" / "ast_lowering.c"
    if ast_lowering_path.exists():
        content = ast_lowering_path.read_text()
        
        print("\n[ast_lowering.c]")
        if "emit_callee_save_loads" in content:
            print("  ⚠️  emit_callee_save_loads() found (should be moved to backend)")
            
            # Check if it's already conditional
            if "c->isa == ISA_X86_64" in content or "c->isa == ISA_AARCH64" in content:
                print("  ✓  Already ISA-conditional (quick fix applied)")
            else:
                print("  ✗  NOT ISA-conditional (needs fix)")
        
        if "callee_save_slot_offset" in content:
            print("  ⚠️  callee_save_slot_offset[] found (x86-64 specific data)")
    
    # Check backend files
    for isa in ['x86_64', 'aarch64', 'riscv']:
        backend_path = base_path / "isa" / f"{isa}.c"
        if backend_path.exists():
            content = backend_path.read_text()
            print(f"\n[{isa}.c]")
            
            if "emit_callee_save_loads" in content:
                print(f"  ✓  emit_callee_save_loads() implemented")
            else:
                print(f"  -  emit_callee_save_loads() not implemented (may be NULL)")
    
    print("\n" + "=" * 70)

def verify_backend_interface():
    """Verify backend interface has the hook."""
    print("=" * 70)
    print("BACKEND INTERFACE VERIFICATION")
    print("=" * 70)
    
    interface_path = Path("src/zero_cc/isa/isa_backend.h")
    if not interface_path.exists():
        print(f"✗ Backend interface not found: {interface_path}")
        return
    
    content = interface_path.read_text()
    
    print("\n[isa_backend.h]")
    
    # Check for callee_save hook
    if "callee_save" in content.lower():
        print("  ✓  Callee-saved hook found in interface")
    else:
        print("  -  Callee-saved hook not in interface (may need to add)")
    
    # List all backend function pointers
    print("\n  Current backend interface functions:")
    func_pattern = r'\s*\(\s*void\s*\(\*\s*(\w+)\s*\)\s*\('
    matches = re.findall(func_pattern, content)
    for func in matches[:15]:  # Show first 15
        print(f"    - {func}")
    
    print("\n" + "=" * 70)

def generate_refactoring_plan():
    """Generate step-by-step refactoring plan."""
    print("=" * 70)
    print("AST LOWERING REFACTORING PLAN")
    print("=" * 70)
    
    plan = """
Step 1: Add backend interface hook
-----------------------------------
File: src/zero_cc/isa/isa_backend.h

Add to ZeroCCBackend struct:
    void (*emit_callee_save_loads)(ZeroCCCompiler *c);

Step 2: Implement in x86_64 backend
------------------------------------
File: src/zero_cc/isa/x86_64.c

Move emit_callee_save_loads() and callee_save_slot_offset[] from
ast_lowering.c to x86_64.c

Update backend initialization:
    BE(c)->emit_callee_save_loads = x86_64_emit_callee_save_loads;

Step 3: Set to NULL for other ISAs
-----------------------------------
File: src/zero_cc/isa/aarch64.c
File: src/zero_cc/isa/riscv.c

Initialize:
    BE(c)->emit_callee_save_loads = NULL;

Step 4: Remove from ast_lowering.c
-----------------------------------
File: src/zero_cc/parser/ast_lowering.c

Remove:
- emit_callee_save_loads() function
- callee_save_slot_offset[] array
- Call to emit_callee_save_loads(c)

Replace with hook call (optional):
    if (BE(c)->emit_callee_save_loads) {
        BE(c)->emit_callee_save_loads(c);
    }

Step 5: Test
------------
- Run all 118 JIT tests
- Verify x86-64 still works correctly
- Verify AArch64 doesn't break
- Verify RISC-V doesn't break

Estimated Time: 2-3 hours
Risk Level: Low (isolated change, easy to revert)
"""
    print(plan)
    print("=" * 70)

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 callee_saved_analyzer.py <command>")
        print()
        print("Commands:")
        print("  --check-isas       Check current ISA implementations")
        print("  --verify-backend   Verify backend interface")
        print("  --generate-report  Generate full refactoring plan")
        print()
        print("Running all checks...")
        check_isa_implementation()
        print()
        verify_backend_interface()
        print()
        generate_refactoring_plan()
        return
    
    cmd = sys.argv[1]
    
    if cmd == "--check-isas":
        check_isa_implementation()
    elif cmd == "--verify-backend":
        verify_backend_interface()
    elif cmd == "--generate-report":
        generate_refactoring_plan()
    else:
        print(f"Unknown command: {cmd}")
        sys.exit(1)

if __name__ == '__main__':
    main()
