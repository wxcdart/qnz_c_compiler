#!/usr/bin/env python3
"""Task 56 Pre-Check - Verify Two-Pass IR Architecture Readiness

Checks:
1. IR opcode completeness (all opcodes have Pass 2 handlers)
2. Backend function coverage (all backends implement required functions)
3. Circular dependency detection
4. IR buffer integration (Pass 1 → Pass 2 connection)
5. Missing test coverage
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
    
    # Scan all .c and .h files for includes
    for root, dirs, files in os.walk('src'):
        for file in files:
            if file.endswith(('.c', '.h')):
                filepath = os.path.join(root, file)
                with open(filepath, 'r') as f:
                    content = f.read()
                    includes = re.findall(r'#include\s+[<"]([^>"]+)[>"]', content)
                    for inc in includes:
                        include_map[filepath].add(inc)
    
    # Check for circular dependencies
    circular = []
    for file, includes in include_map.items():
        for inc in includes:
            # Normalize paths
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

def check_ir_opcode_coverage():
    """Check if all IR opcodes have Pass 2 handlers"""
    print("\n" + "=" * 70)
    print("2. IR OPCODE PASS 2 COVERAGE")
    print("=" * 70)
    
    # Read ir.h to get all opcodes
    ir_h_path = 'src/zero_cc/parser/ir.h'
    if not os.path.exists(ir_h_path):
        print(f"❌ File not found: {ir_h_path}")
        return False
    
    with open(ir_h_path, 'r') as f:
        ir_content = f.read()
    
    # Extract opcode names
    opcodes = re.findall(r'OP_(\w+),', ir_content)
    print(f"Found {len(opcodes)} opcodes in ir.h")
    
    # Read ast_lowering_mc.c for Pass 2 handlers
    mc_path = 'src/zero_cc/parser/ast_lowering_mc.c'
    if not os.path.exists(mc_path):
        print(f"❌ File not found: {mc_path}")
        return False
    
    with open(mc_path, 'r') as f:
        mc_content = f.read()
    
    # Check for case statements handling each opcode
    missing = []
    covered = []
    
    for opcode in opcodes:
        pattern = rf'case\s+OP_{opcode}:'
        if re.search(pattern, mc_content):
            covered.append(opcode)
        else:
            missing.append(opcode)
    
    print(f"✅ Covered: {len(covered)} opcodes")
    print(f"❌ Missing: {len(missing)} opcodes")
    
    if missing:
        print("\nMissing opcode handlers:")
        for op in missing[:20]:
            print(f"   - OP_{op}")
        if len(missing) > 20:
            print(f"   ... and {len(missing) - 20} more")
    
    return len(missing) == 0

def check_backend_functions():
    """Check if all backends implement required functions"""
    print("\n" + "=" * 70)
    print("3. BACKEND FUNCTION COVERAGE")
    print("=" * 70)
    
    # Required functions from isa_backend.h
    required = [
        'emit_prologue', 'emit_leave', 'emit_epilogue', 'get_code_offset',
        'emit_mov_imm', 'emit_add', 'emit_sub', 'emit_mul', 'emit_div',
        'emit_mod', 'emit_and', 'emit_or', 'emit_xor', 'emit_shl',
        'emit_cmp', 'emit_neg', 'emit_test', 'emit_cqo', 'emit_idiv',
        'emit_lea', 'patch_jmp', 'resolve_labels', 'emit_label',
        'get_int_reg', 'get_fp_reg'
    ]
    
    backend_files = [
        'src/zero_cc/isa/x86_64.c',
        'src/zero_cc/isa/aarch64.c',
        'src/zero_cc/isa/riscv.c',
    ]
    
    all_ok = True
    
    for backend_file in backend_files:
        if not os.path.exists(backend_file):
            print(f"❌ Backend not found: {backend_file}")
            all_ok = False
            continue
        
        with open(backend_file, 'r') as f:
            content = f.read()
        
        backend_name = os.path.basename(backend_file)
        missing = []
        
        for func in required:
            if f'{func} =' not in content and f'->{func}' not in content:
                # Check if function is defined
                if not re.search(rf'static\s+void\s+{backend_name.replace(".c", "")}_{func}', content):
                    missing.append(func)
        
        if missing:
            print(f"⚠️  {backend_name}: {len(missing)} functions missing/unchecked")
            all_ok = False
        else:
            print(f"✅ {backend_name}: All required functions present")
    
    return all_ok

def check_ir_buffer_integration():
    """Check if IR buffer is properly integrated"""
    print("\n" + "=" * 70)
    print("4. IR BUFFER INTEGRATION")
    print("=" * 70)
    
    checks = {
        'ir_get_buffer': False,
        'ir_get_count': False,
        'ir_emit': False,
        'emit_ir_to_machine_code': False,
        'ir_clear': False,
    }
    
    files_to_check = [
        'src/zero_cc/parser/ir.h',
        'src/zero_cc/parser/ir.c',
        'src/zero_cc/parser/ast_lowering_mc.c',
        'src/zero_cc/parser/ast_lowering.c',
    ]
    
    for filepath in files_to_check:
        if not os.path.exists(filepath):
            continue
        with open(filepath, 'r') as f:
            content = f.read()
        for check in checks:
            if check in content:
                checks[check] = True
    
    all_ok = True
    for check, found in checks.items():
        status = "✅" if found else "❌"
        print(f"{status} {check}: {'Found' if found else 'Missing'}")
        if not found:
            all_ok = False
    
    return all_ok

def check_two_pass_flow():
    """Check the two-pass flow from AST → IR → Machine Code"""
    print("\n" + "=" * 70)
    print("5. TWO-PASS FLOW INTEGRATION")
    print("=" * 70)
    
    # Check ast_lowering.c for the main flow (not parser.c)
    ast_lowering_c = 'src/zero_cc/parser/ast_lowering.c'
    if not os.path.exists(ast_lowering_c):
        print(f"❌ File not found: {ast_lowering_c}")
        return False
    
    with open(ast_lowering_c, 'r') as f:
        content = f.read()
    
    checks = {
        'zero_cc_lower_ast': 'AST → IR (Pass 1)',
        'emit_ir_to_machine_code': 'IR → MC (Pass 2)',
        'regalloc': 'Register allocation',
    }
    
    all_ok = True
    for check, desc in checks.items():
        if check in content:
            print(f"✅ {desc}: Found in ast_lowering.c")
        else:
            print(f"⚠️  {desc}: Not found in ast_lowering.c")
            all_ok = False
    
    return all_ok

def main():
    print("=" * 70)
    print("Task 56 Pre-Check - Two-Pass IR Architecture Readiness")
    print("=" * 70)
    print()
    
    results = {
        'Circular Dependencies': check_circular_dependencies(),
        'IR Opcode Coverage': check_ir_opcode_coverage(),
        'Backend Functions': check_backend_functions(),
        'IR Buffer Integration': check_ir_buffer_integration(),
        'Two-Pass Flow': check_two_pass_flow(),
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
        print("✅ Task 56 READY TO START - All pre-checks passed!")
    else:
        print("⚠️  Task 56 NOT READY - Fix failing pre-checks first")
    print("=" * 70)
    
    return all_passed

if __name__ == '__main__':
    success = main()
    exit(0 if success else 1)
