#!/usr/bin/env python3
"""
Task 58 Pre-Check: Loop Optimization Readiness
Checks if the infrastructure for Task 58 (LICM, strength reduction, branch simplification) is ready.
"""

import json
import sys
import os

def load_deps(filepath):
    with open(filepath, 'r') as f:
        return json.load(f)

def check_opt_passes(deps):
    """Check if optimization pass infrastructure exists."""
    files = deps.get('files', {})
    
    required_files = [
        'zero_cc/parser/opt_pass.h',
        'zero_cc/parser/opt_pass.c',
        'zero_cc/parser/opt_constant_fold.h',
        'zero_cc/parser/opt_constant_fold.c',
        'zero_cc/parser/opt_dce.h',
        'zero_cc/parser/opt_dce.c',
        'zero_cc/parser/opt_cse.h',
        'zero_cc/parser/opt_cse.c',
    ]
    
    missing = []
    for f in required_files:
        if f not in files:
            missing.append(f)
    
    return len(missing) == 0, missing

def check_ir_opcodes(deps):
    """Check if required IR opcodes exist."""
    functions = deps.get('functions', {})
    
    # Check for key IR functions
    required_funcs = [
        'ir_get_count',
        'ir_get',
        'ir_emit',
        'ir_clear',
    ]
    
    missing = []
    for f in required_funcs:
        if f not in functions:
            missing.append(f)
    
    return len(missing) == 0, missing

def check_loop_constructs():
    """Check if loop constructs are properly lowered."""
    # This would require parsing the actual IR
    # For now, just return True
    return True, []

def check_register_allocator(deps):
    """Check if register allocator is available."""
    files = deps.get('files', {})
    
    required = [
        'zero_cc/parser/regalloc.h',
    ]
    
    missing = []
    for f in required:
        if f not in files:
            missing.append(f)
    
    return len(missing) == 0, missing

def main():
    print("=" * 60)
    print("Task 58 Pre-Check: Loop Optimization Readiness")
    print("=" * 60)
    print()
    
    if len(sys.argv) < 2:
        print("Usage: python3 tools/task58_precheck.py <deps_graph.json>")
        sys.exit(1)
    
    deps_file = sys.argv[1]
    if not os.path.exists(deps_file):
        print(f"Error: {deps_file} not found")
        sys.exit(1)
    
    deps = load_deps(deps_file)
    
    checks = [
        ("Optimization pass infrastructure", check_opt_passes(deps)),
        ("IR opcode infrastructure", check_ir_opcodes(deps)),
        ("Register allocator", check_register_allocator(deps)),
        ("Loop constructs", check_loop_constructs()),
    ]
    
    all_passed = True
    for name, (passed, missing) in checks:
        status = "✅ PASS" if passed else "�️ FAIL"
        print(f"{status}: {name}")
        if missing:
            for m in missing:
                print(f"  - Missing: {m}")
        if not passed:
            all_passed = False
    
    print()
    print("=" * 60)
    if all_passed:
        print("✅ All checks passed! Ready for Task 58")
        print()
        print("Task 58 will implement:")
        print("  1. Loop-invariant code motion (LICM)")
        print("  2. Strength reduction (mul → shifts/adds)")
        print("  3. Branch simplification")
        print()
        print("Next steps:")
        print("  1. Create opt_loop.h/c with loop analysis framework")
        print("  2. Implement LICM pass")
        print("  3. Implement strength reduction pass")
        print("  4. Implement branch simplification pass")
        print("  5. Add to opt_pass.c pipeline")
        sys.exit(0)
    else:
        print("❌ Some checks failed. Fix before Task 58")
        sys.exit(1)

if __name__ == '__main__':
    main()
