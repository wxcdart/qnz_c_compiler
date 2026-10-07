#!/usr/bin/env python3
"""
ISA Readiness Summary

Quick summary of what's needed to support each ISA.
"""

import subprocess
import sys
from pathlib import Path

def count_functions(filepath: Path, prefix: str) -> int:
    """Count emit_* functions in backend."""
    if not filepath.exists():
        return 0
    with open(filepath, 'r') as f:
        content = f.read()
    return content.count(f"static void {prefix}_emit_")

def main():
    root = Path.cwd()
    
    backends = {
        "x86_64": root / "src/zero_cc/isa/x86_64.c",
        "aarch64": root / "src/zero_cc/isa/aarch64.c",
        "riscv64": root / "src/zero_cc/isa/riscv.c",
    }
    
    print(f"\n{'='*70}")
    print(f"ISA Backend Readiness Summary")
    print(f"{'='*70}\n")
    
    for isa, filepath in backends.items():
        func_count = count_functions(filepath, isa.replace('_', ''))
        status = "✓ Ready" if func_count > 30 else "⚠ Incomplete" if func_count > 10 else "✗ Missing"
        
        print(f"{isa}:")
        print(f"  Status: {status}")
        print(f"  Functions: {func_count}")
        print()
    
    # Check IR for x86 hardcoding
    ir_file = root / "src/zero_cc/parser/ir.h"
    with open(ir_file, 'r') as f:
        ir_content = f.read()
    
    x86_opcodes = ['OP_PUSH_RAX', 'OP_POP_RAX', 'OP_LEA_RAX_MEM', 'OP_MOV_RAX_RBX', 'OP_CQO']
    x86_count = sum(1 for op in x86_opcodes if op in ir_content)
    
    print(f"IR Layer:")
    print(f"  x86-specific opcodes: {x86_count}")
    print(f"  Recommendation: Refactor to ISA-neutral opcodes")
    print()
    
    print(f"{'='*70}")
    print(f"Recommendation:")
    print(f"  1. Fix IR hardcoding (2-3 weeks)")
    print(f"  2. Complete AArch64 backend (2-3 weeks)")
    print(f"  3. THEN add SIMD (3-4 weeks)")
    print(f"{'='*70}\n")

if __name__ == "__main__":
    main()
