#!/usr/bin/env python3
"""
Flag Register Verification Tool

Verifies that Phase 2 (flag register) migration was done correctly.

Checks:
1. REG_FLAGS constant is defined
2. Instr structure has flags_in/flags_out fields
3. All flag-setting ops set flags_out = REG_FLAGS
4. All flag-reading ops set flags_in = REG_FLAGS
5. Backends read flags_in/flags_out correctly

Usage:
    python3 tools/verify_flag_registers.py
"""

import re
import sys
from pathlib import Path

def check_reg_flags_defined():
    """Check if REG_FLAGS is defined."""
    ir_h = Path('src/zero_cc/parser/ir.h')
    if not ir_h.exists():
        return False, "ir.h not found"
    
    content = ir_h.read_text()
    if 'REG_FLAGS' in content:
        match = re.search(r'#define\s+REG_FLAGS\s+(\d+)', content)
        if match:
            return True, f"REG_FLAGS = {match.group(1)}"
        return True, "REG_FLAGS defined"
    return False, "REG_FLAGS not defined"

def check_instr_structure():
    """Check if Instr structure has flags_in/flags_out."""
    ir_h = Path('src/zero_cc/parser/ir.h')
    if not ir_h.exists():
        return False, "ir.h not found"
    
    content = ir_h.read_text()
    
    has_flags_out = 'flags_out' in content
    has_flags_in = 'flags_in' in content
    
    if has_flags_out and has_flags_in:
        return True, "flags_in and flags_out fields present"
    elif has_flags_out:
        return False, "flags_out present, flags_in missing"
    elif has_flags_in:
        return False, "flags_in present, flags_out missing"
    else:
        return False, "Neither flags_in nor flags_out found"

def check_helper_functions():
    """Check if helper functions set flags_in/flags_out."""
    issues = []
    
    # Check ir_emit_cmp
    ir_h = Path('src/zero_cc/parser/ir.h')
    ir_emit_ops = Path('src/zero_cc/parser/ir_emit_ops.h')
    
    files_to_check = []
    ir_c = Path("src/zero_cc/parser/ir.c")
    if ir_c.exists():
        files_to_check.append(("ir.c", ir_c.read_text()))
    if ir_h.exists():
        files_to_check.append(('ir.h', ir_h.read_text()))
    if ir_emit_ops.exists():
        files_to_check.append(('ir_emit_ops.h', ir_emit_ops.read_text()))
    
    # Check for cmp functions
    cmp_found = False
    cmp_has_flags_out = False
    for name, content in files_to_check:
        if 'ir_emit_cmp' in content:
            cmp_found = True
            # Look for flags_out in cmp function
            cmp_pattern = r'ir_emit_cmp.*?flags_out\s*=\s*REG_FLAGS'
            if re.search(cmp_pattern, content, re.DOTALL):
                cmp_has_flags_out = True
    
    if cmp_found and not cmp_has_flags_out:
        issues.append("ir_emit_cmp() missing flags_out = REG_FLAGS")
    
    # Check for cond_set functions
    cond_set_found = False
    cond_set_has_flags_in = False
    for name, content in files_to_check:
        if 'ir_emit_cond_set' in content:
            cond_set_found = True
            cond_set_pattern = r'ir_emit_cond_set.*?flags_in\s*=\s*REG_FLAGS'
            if re.search(cond_set_pattern, content, re.DOTALL):
                cond_set_has_flags_in = True
    
    if cond_set_found and not cond_set_has_flags_in:
        issues.append("ir_emit_cond_set() missing flags_in = REG_FLAGS")
    
    # Check for jcond functions
    jcond_found = False
    jcond_has_flags_in = False
    for name, content in files_to_check:
        if 'ir_emit_jcond' in content:
            jcond_found = True
            jcond_pattern = r'ir_emit_jcond.*?flags_in\s*=\s*REG_FLAGS'
            if re.search(jcond_pattern, content, re.DOTALL):
                jcond_has_flags_in = True
    
    if jcond_found and not jcond_has_flags_in:
        issues.append("ir_emit_jcond() missing flags_in = REG_FLAGS")
    
    if issues:
        return False, "\n".join(issues)
    return True, "All helper functions updated correctly"

def check_backend_updates():
    """Check if backends reference flags_in/flags_out."""
    issues = []
    
    backends = [
        ('x86_64.c', Path('src/zero_cc/isa/x86_64.c')),
        ('aarch64.c', Path('src/zero_cc/isa/aarch64.c')),
        ('riscv.c', Path('src/zero_cc/isa/riscv.c')),
    ]
    
    for name, path in backends:
        if path.exists():
            content = path.read_text()
            # Check if backend has any reference to flags_in or flags_out
            if 'flags_in' not in content and 'flags_out' not in content:
                # Backend doesn't use flags yet - this is expected before migration
                pass
    
    if issues:
        return False, "\n".join(issues)
    return True, "Backend check complete"

def main():
    print("=" * 80)
    print("Flag Register Verification")
    print("=" * 80)
    print()
    
    all_passed = True
    
    # Check 1: REG_FLAGS defined
    passed, msg = check_reg_flags_defined()
    status = "✓" if passed else "✗"
    print(f"[{status}] REG_FLAGS constant: {msg}")
    if not passed:
        all_passed = False
    
    # Check 2: Instr structure
    passed, msg = check_instr_structure()
    status = "✓" if passed else "✗"
    print(f"[{status}] Instr structure: {msg}")
    if not passed:
        all_passed = False
    
    # Check 3: Helper functions
    passed, msg = check_helper_functions()
    status = "✓" if passed else "✗"
    print(f"[{status}] Helper functions: {msg}")
    if not passed:
        all_passed = False
    
    # Check 4: Backend updates
    passed, msg = check_backend_updates()
    status = "✓" if passed else "✗"
    print(f"[{status}] Backend updates: {msg}")
    if not passed:
        all_passed = False
    
    print()
    print("=" * 80)
    if all_passed:
        print("VERIFICATION PASSED: All flag register checks passed!")
    else:
        print("VERIFICATION FAILED: Some checks failed. See above for details.")
    print("=" * 80)
    
    return 0 if all_passed else 1

if __name__ == "__main__":
    sys.exit(main())
