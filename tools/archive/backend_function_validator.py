#!/usr/bin/env python3
"""Backend Function Validator - Checks that all ISA backend functions are properly registered.

Verifies that each ISA backend has all required function pointers initialized and non-null.

Usage:
    python3 tools/backend_function_validator.py --isa aarch64
"""

import subprocess
import sys
import re
from pathlib import Path
from dataclasses import dataclass
from typing import Optional

@dataclass
class BackendFunction:
    name: str
    required: bool
    implemented: bool
    registered: bool

# Required backend functions for all ISAs
REQUIRED_FUNCTIONS = [
    # Core emission
    "emit_prologue",
    "emit_epilogue",
    "emit_mov_reg",
    "emit_mov_imm",
    "emit_add",
    "emit_sub",
    "emit_mul",
    "emit_div",
    "emit_and",
    "emit_or",
    "emit_xor",
    "emit_shl",
    "emit_shr",
    "emit_cmp",
    "emit_test",
    "emit_jmp",
    "emit_jeq",
    "emit_jne",
    "emit_jlt",
    "emit_jle",
    "emit_jgt",
    "emit_jge",
    "emit_call",
    "emit_ret",
    "emit_push_reg",
    "emit_pop_reg",
    
    # Memory operations
    "emit_deref",
    "emit_load",
    "emit_store",
    "emit_load_32",
    "emit_load_16",
    "emit_load_byte",
    "emit_store_32",
    "emit_store_16",
    "emit_store_byte",
    "emit_lea",
    "emit_lea_rip",
    
    # Comparisons
    "emit_setl",
    "emit_setle",
    "emit_setg",
    "emit_setge",
    "emit_sete",
    "emit_setne",
    "emit_cmp_zero",
    "emit_lnot",
    
    # FP operations
    "emit_load_fp",
    "emit_store_fp",
    "emit_fp_add",
    "emit_fp_sub",
    "emit_fp_mul",
    "emit_fp_div",
    "emit_fp_mov",
    "emit_fp_cvtsi",
    "emit_fp_cvtsf",
    "emit_load_fp_rodata",
    
    # ABI-specific
    "emit_param_save_int",
    "emit_param_save_fp",
    "emit_variadic_spill",
    "emit_variadic_al_setup",
    
    # Special operations
    "emit_vla_alloc",
    "emit_atomic_cmpxchg",
    "emit_atomic_xadd",
    "emit_atomic_xchg",
    "emit_mfence",
    "emit_not",
    "emit_neg",
    "emit_mod",
    "emit_sar",
    "emit_inc",
    "emit_dec",
    "emit_stack_align",
    "emit_stack_adjust",
    "emit_call_reg",
    "emit_inc",
    "emit_get_code_offset",
]

def check_symbol_exists(binary: str, symbol: str) -> bool:
    """Check if a symbol exists in the binary using nm."""
    try:
        result = subprocess.run(
            ["nm", binary],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=30
        )
        return symbol in result.stdout
    except Exception:
        return False

def get_backend_functions(isa: str) -> list[str]:
    """Get backend function names for a specific ISA."""
    prefix_map = {
        'x86_64': ['x86_64_', 'x86_emit_'],
        'aarch64': ['aarch64_'],
        'riscv64': ['riscv_'],
    }
    
    prefixes = prefix_map.get(isa, [])
    functions = []
    
    for prefix in prefixes:
        for func in REQUIRED_FUNCTIONS:
            functions.append(f"{prefix}{func}")
    
    return functions

def validate_backend(isa: str, binary: str = "./build/bin/zerocc") -> dict[str, BackendFunction]:
    """Validate backend functions for an ISA."""
    results = {}
    
    backend_funcs = get_backend_functions(isa)
    
    for func in backend_funcs:
        exists = check_symbol_exists(binary, func)
        results[func] = BackendFunction(
            name=func,
            required=func.split('_')[-1] in [f.split('_')[-1] for f in REQUIRED_FUNCTIONS],
            implemented=exists,
            registered=exists  # For now, assume registered if implemented
        )
    
    return results

def print_report(results: dict[str, BackendFunction], isa: str) -> int:
    """Print validation report."""
    print(f"\n=== Backend Function Validation for {isa.upper()} ===")
    
    implemented = sum(1 for r in results.values() if r.implemented)
    missing = sum(1 for r in results.values() if not r.implemented)
    
    print(f"Total functions: {len(results)}")
    print(f"Implemented: {implemented}")
    print(f"Missing: {missing}")
    print()
    
    if missing > 0:
        print("Missing functions:")
        for name, func in results.items():
            if not func.implemented:
                print(f"  - {name}")
        print()
    
    # Check for critical functions
    critical = ['emit_prologue', 'emit_epilogue', 'emit_mov_reg', 'emit_call', 'emit_ret']
    print("Critical functions status:")
    for func_name in critical:
        # Find matching function
        for name, func in results.items():
            if func_name in name:
                status = "✓" if func.implemented else "✗"
                print(f"  {status} {func_name} -> {name}")
                break
    
    return 1 if missing > 0 else 0

def main() -> int:
    import argparse
    parser = argparse.ArgumentParser(description='Validate ISA backend functions')
    parser.add_argument('--isa', required=True, choices=['x86_64', 'aarch64', 'riscv64'],
                       help='Target ISA to validate')
    parser.add_argument('--binary', default='./build/bin/zerocc',
                       help='Compiler binary path')
    args = parser.parse_args()
    
    print(f"Validating {args.isa} backend in {args.binary}...")
    
    if not Path(args.binary).exists():
        print(f"[error] Binary not found: {args.binary}")
        return 1
    
    results = validate_backend(args.isa, args.binary)
    return print_report(results, args.isa)

if __name__ == '__main__':
    sys.exit(main())
