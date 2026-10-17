#!/usr/bin/env python3
"""Backend Initialization Checker - Verifies ISA backend initialization in main.c.

Checks that backend function pointers are properly registered for each ISA.

Usage:
    python3 tools/backend_init_checker.py
"""

import re
from pathlib import Path
from dataclasses import dataclass
from typing import Optional

@dataclass
class BackendRegistration:
    isa: str
    prologue_func: Optional[str]
    epilogue_func: Optional[str]
    mov_reg_func: Optional[str]
    call_func: Optional[str]
    all_registered: bool

def parse_backend_struct(source: Path, isa: str) -> Optional[BackendRegistration]:
    """Parse backend structure initialization for an ISA."""
    content = source.read_text()
    
    # Look for backend initialization patterns specific to this ISA
    if isa == 'x86_64':
        patterns = {
            'prologue': r'x86_emit_prologue',
            'epilogue': r'x86_emit_epilogue',
            'mov_reg': r'x86_emit_mov_reg',
            'call': r'x86_emit_call',
        }
    elif isa == 'aarch64':
        patterns = {
            'prologue': r'aarch64_emit_prologue',
            'epilogue': r'aarch64_emit_epilogue',
            'mov_reg': r'aarch64_emit_mov_reg',
            'call': r'aarch64_emit_call',
        }
    elif isa == 'riscv':
        patterns = {
            'prologue': r'riscv.*_emit_prologue',
            'epilogue': r'riscv.*_emit_epilogue',
            'mov_reg': r'riscv.*_emit_mov_reg',
            'call': r'riscv.*_emit_call',
        }
    else:
        return None
    
    results = {}
    for key, pattern in patterns.items():
        match = re.search(pattern, content)
        results[key] = match.group(0) if match else None
    
    all_reg = all(v is not None for v in results.values())
    
    return BackendRegistration(
        isa=isa,
        prologue_func=results['prologue'],
        epilogue_func=results['epilogue'],
        mov_reg_func=results['mov_reg'],
        call_func=results['call'],
        all_registered=all_reg
    )

def check_backend_table(source: Path) -> list[BackendRegistration]:
    """Check all backend registrations in source file."""
    isas = ['x86_64', 'aarch64', 'riscv']
    results = []
    
    for isa in isas:
        reg = parse_backend_struct(source, isa)
        if reg:
            results.append(reg)
    
    return results

def check_all_backend_files() -> list[BackendRegistration]:
    """Check backend registrations in all ISA files."""
    isa_dir = Path("src/zero_cc/isa")
    all_results = []
    
    if not isa_dir.exists():
        return all_results
    
    for isa_file in isa_dir.glob("*.c"):
        results = check_backend_table(isa_file)
        all_results.extend(results)
    
    return all_results

def analyze_backend_code(source: Path) -> dict:
    """Analyze backend-related code patterns."""
    content = source.read_text()
    
    analysis = {
        'backend_structs': len(re.findall(r'ZeroCCISABackend\s+\w+\s*=', content)),
        'isa_enum_values': len(re.findall(r'ISA_\w+', content)),
        'backend_init_calls': len(re.findall(r'zero_cc_get_backend|zero_cc_init_compiler_with_target', content)),
        'isa_checks': len(re.findall(r'c->isa\s*==\s*ISA_\w+', content)),
    }
    
    return analysis

def main() -> int:
    print("=== Backend Initialization Checker ===")
    print()
    
    main_c = Path("src/main.c")
    isa_backend_h = Path("src/zero_cc/isa/isa_backend.h")
    
    # Check main.c
    if main_c.exists():
        print(f"Analyzing {main_c}...")
        analysis = analyze_backend_code(main_c)
        
        print(f"  Backend structs found: {analysis['backend_structs']}")
        print(f"  ISA enum values: {analysis['isa_enum_values']}")
        print(f"  Backend init calls: {analysis['backend_init_calls']}")
        print(f"  ISA checks: {analysis['isa_checks']}")
        print()
        
        # Parse backend registrations from all ISA files
        print("Backend Registrations (from ISA files):")
        registrations = check_all_backend_files()
        
        for reg in registrations:
            status = "✓" if reg.all_registered else "✗"
            print(f"  {status} {reg.isa.upper()}")
            print(f"      prologue: {reg.prologue_func or 'MISSING'}")
            print(f"      epilogue: {reg.epilogue_func or 'MISSING'}")
            print(f"      mov_reg:  {reg.mov_reg_func or 'MISSING'}")
            print(f"      call:     {reg.call_func or 'MISSING'}")
            print()
    else:
        print(f"[warning] {main_c} not found")
    
    # Check isa_backend.h
    if isa_backend_h.exists():
        print(f"Analyzing {isa_backend_h}...")
        content = isa_backend_h.read_text()
        
        # Count function pointers in struct
        func_pointers = re.findall(r'void\s+\(\*\s*(\w+)\s*\)', content)
        print(f"  Function pointer types in struct: {len(func_pointers)}")
        
        # List them
        if func_pointers:
            print("  Functions:")
            for fp in func_pointers[:10]:  # Show first 10
                print(f"    - {fp}")
            if len(func_pointers) > 10:
                print(f"    ... and {len(func_pointers) - 10} more")
    else:
        print(f"[warning] {isa_backend_h} not found")
    
    print()
    print("=== Recommendations ===")
    
    # Check for common issues
    if main_c.exists():
        content = main_c.read_text()
        
        if 'aarch64' in content.lower() and 'aarch64_emit_prologue' not in content:
            print("⚠ AArch64 prologue function may not be registered")
        
        if 'ISA_AARCH64' in content and 'aarch64' not in content.lower():
            print("⚠ ISA_AARCH64 defined but AArch64 backend may not be initialized")
        
        # Check for null checks
        null_checks = len(re.findall(r'if\s*\(\s*BE\s*\(\s*c\s*\)\s*->', content))
        if null_checks < 5:
            print(f"⚠ Only {null_checks} backend function pointer checks found (recommend more)")
    
    return 0

if __name__ == '__main__':
    import sys
    sys.exit(main())
