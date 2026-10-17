#!/usr/bin/env python3
"""Task 57 Pre-Check - Optimization Pipeline Readiness

Checks:
1. IR opcode completeness for optimizations
2. Constant folding infrastructure
3. DCE (Dead Code Elimination) infrastructure
4. CSE (Common Subexpression Elimination) infrastructure
5. Optimization pass framework
6. Circular dependency analysis
7. Test coverage for optimization verification
"""

import os
import re
from collections import defaultdict

def check_circular_dependencies():
    """Check for circular include dependencies"""
    print("=" * 70)
    print("1. CIRCULAR DEPENDENCY ANALYSIS")
    print("=" * 70)
    
    include_map = defaultdict(set)
    
    for root, dirs, files in os.walk('src'):
        for file in files:
            if file.endswith(('.c', '.h')):
                filepath = os.path.join(root, file)
                with open(filepath, 'r') as f:
                    content = f.read()
                    includes = re.findall(r'#include\s+[<"]([^>"]+)[>"]', content)
                    for inc in includes:
                        include_map[filepath].add(inc)
    
    circular = []
    for file, includes in include_map.items():
        for inc in includes:
            inc_path = os.path.join('src', inc) if not inc.startswith('src') else inc
            if inc_path in include_map and file in include_map[inc_path]:
                pair = tuple(sorted([file, inc_path]))
                if pair not in circular:
                    circular.append(pair)
    
    if circular:
        print(f"⚠️  Found {len(circular)} potential circular dependencies:")
        for a, b in circular:
            print(f"   {a} ↔ {b}")
    else:
        print("✅ No circular dependencies found")
    
    return len(circular) == 0

def check_optimization_infrastructure():
    """Check for optimization framework infrastructure"""
    print("\n" + "=" * 70)
    print("2. OPTIMIZATION INFRASTRUCTURE")
    print("=" * 70)
    
    checks = {
        'opt_pass.h': False,
        'opt_pass.c': False,
        'constant_fold.h': False,
        'constant_fold.c': False,
        'dce.h': False,
        'dce.c': False,
        'cse.h': False,
        'cse.c': False,
    }
    
    for filepath in checks.keys():
        path = f'src/zero_cc/parser/{filepath}'
        if os.path.exists(path):
            checks[filepath] = True
    
    for check, found in checks.items():
        status = "✅" if found else "❌"
        print(f"{status} {check}: {'Found' if found else 'Missing'}")
    
    return all(checks.values())

def check_ir_for_optimizations():
    """Check IR opcode coverage for optimizations"""
    print("\n" + "=" * 70)
    print("3. IR OPCODE COVERAGE FOR OPTIMIZATIONS")
    print("=" * 70)
    
    ir_h_path = 'src/zero_cc/parser/ir.h'
    if not os.path.exists(ir_h_path):
        print(f"❌ File not found: {ir_h_path}")
        return False
    
    with open(ir_h_path, 'r') as f:
        ir_content = f.read()
    
    # Check for opcodes needed for optimizations
    required_opcodes = [
        'OP_ADD', 'OP_SUB', 'OP_MUL', 'OP_DIV',  # Arithmetic for folding
        'OP_AND', 'OP_OR', 'OP_XOR',  # Bitwise for folding
        'OP_SHL', 'OP_SHR',  # Shifts for strength reduction
        'OP_NEG', 'OP_NOT',  # Unary ops for folding
        'OP_CMP',  # Comparisons for DCE
        'OP_MOV',  # Moves for CSE
        'OP_CONST',  # Constants for folding
    ]
    
    found = []
    missing = []
    
    for op in required_opcodes:
        if f'OP_{op}' in ir_content:
            found.append(op)
        else:
            missing.append(op)
    
    print(f"✅ Found: {len(found)} opcodes")
    print(f"❌ Missing: {len(missing)} opcodes")
    
    if missing:
        print("\nMissing opcodes:")
        for op in missing:
            print(f"   - OP_{op}")
    
    return len(missing) == 0

def check_constant_folding():
    """Check constant folding infrastructure"""
    print("\n" + "=" * 70)
    print("4. CONSTANT FOLDING INFRASTRUCTURE")
    print("=" * 70)
    
    checks = {
        'ir_fold_constants': False,
        'opt_fold_int': False,
        'opt_fold_float': False,
    }
    
    files_to_check = [
        'src/zero_cc/parser/ir.h',
        'src/zero_cc/parser/ir.c',
        'src/zero_cc/parser/ast_lowering_mc.c',
    ]
    
    for filepath in files_to_check:
        if not os.path.exists(filepath):
            continue
        with open(filepath, 'r') as f:
            content = f.read()
        for check in checks:
            if check in content:
                checks[check] = True
    
    for check, found in checks.items():
        status = "✅" if found else "❌"
        print(f"{status} {check}: {'Found' if found else 'Missing'}")
    
    return all(checks.values())

def check_dce_infrastructure():
    """Check DCE (Dead Code Elimination) infrastructure"""
    print("\n" + "=" * 70)
    print("5. DCE INFRASTRUCTURE")
    print("=" * 70)
    
    checks = {
        'ir_eliminate_dead': False,
        'opt_dce': False,
        'live_analysis': False,
    }
    
    files_to_check = [
        'src/zero_cc/parser/ir.h',
        'src/zero_cc/parser/ir.c',
        'src/zero_cc/parser/regalloc.h',
    ]
    
    for filepath in files_to_check:
        if not os.path.exists(filepath):
            continue
        with open(filepath, 'r') as f:
            content = f.read()
        for check in checks:
            if check in content:
                checks[check] = True
    
    for check, found in checks.items():
        status = "✅" if found else "❌"
        print(f"{status} {check}: {'Found' if found else 'Missing'}")
    
    return all(checks.values())

def check_cse_infrastructure():
    """Check CSE (Common Subexpression Elimination) infrastructure"""
    print("\n" + "=" * 70)
    print("6. CSE INFRASTRUCTURE")
    print("=" * 70)
    
    checks = {
        'ir_eliminate_cse': False,
        'opt_cse': False,
        'hash_table_for_cse': False,
    }
    
    files_to_check = [
        'src/zero_cc/parser/ir.h',
        'src/zero_cc/parser/ir.c',
        'src/zero_cc/common/hash_table_tpht.h',
    ]
    
    for filepath in files_to_check:
        if not os.path.exists(filepath):
            continue
        with open(filepath, 'r') as f:
            content = f.read()
        for check in checks:
            if check in content:
                checks[check] = True
    
    for check, found in checks.items():
        status = "✅" if found else "❌"
        print(f"{status} {check}: {'Found' if found else 'Missing'}")
    
    return all(checks.values())

def check_optimization_passes():
    """Check optimization pass framework"""
    print("\n" + "=" * 70)
    print("7. OPTIMIZATION PASS FRAMEWORK")
    print("=" * 70)
    
    checks = {
        'opt_run_pass': False,
        'opt_pass_manager': False,
        'opt_level_config': False,
    }
    
    files_to_check = [
        'src/zero_cc/parser/ir.h',
        'src/zero_cc/parser/compile.h',
        'src/zero_cc/jit/compile.h',
    ]
    
    for filepath in files_to_check:
        if not os.path.exists(filepath):
            continue
        with open(filepath, 'r') as f:
            content = f.read()
        for check in checks:
            if check in content:
                checks[check] = True
    
    for check, found in checks.items():
        status = "✅" if found else "❌"
        print(f"{status} {check}: {'Found' if found else 'Missing'}")
    
    return all(checks.values())

def check_test_coverage():
    """Check test coverage for optimization verification"""
    print("\n" + "=" * 70)
    print("8. TEST COVERAGE FOR OPTIMIZATIONS")
    print("=" * 70)
    
    test_dirs = ['test/unit', 'test/jit', 'test/optimization']
    
    opt_tests = {
        'constant_fold': False,
        'dce_test': False,
        'cse_test': False,
        'strength_reduction': False,
    }
    
    for test_dir in test_dirs:
        if not os.path.exists(test_dir):
            continue
        for test_file in os.listdir(test_dir):
            for opt_test in opt_tests.keys():
                if opt_test in test_file:
                    opt_tests[opt_test] = True
    
    for check, found in opt_tests.items():
        status = "✅" if found else "❌"
        print(f"{status} {check}: {'Found' if found else 'Missing'}")
    
    return all(opt_tests.values())

def main():
    print("=" * 70)
    print("Task 57 Pre-Check - Optimization Pipeline Readiness")
    print("=" * 70)
    print()
    
    results = {
        'Circular Dependencies': check_circular_dependencies(),
        'Optimization Infrastructure': check_optimization_infrastructure(),
        'IR Opcode Coverage': check_ir_for_optimizations(),
        'Constant Folding': check_constant_folding(),
        'DCE Infrastructure': check_dce_infrastructure(),
        'CSE Infrastructure': check_cse_infrastructure(),
        'Optimization Pass Framework': check_optimization_passes(),
        'Test Coverage': check_test_coverage(),
    }
    
    print("\n" + "=" * 70)
    print("SUMMARY")
    print("=" * 70)
    
    for check, passed in results.items():
        status = "✅ PASS" if passed else "❌ FAIL"
        print(f"{status}: {check}")
    
    all_passed = all(results.values())
    print("\n" + "=" * 70)
    if all_passed:
        print("✅ Task 57 READY TO START - All pre-checks passed!")
    else:
        print("⚠️  Task 57 NOT READY - Fix failing pre-checks first")
    print("=" * 70)
    
    return all_passed

if __name__ == '__main__':
    success = main()
    exit(0 if success else 1)
