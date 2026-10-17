#!/usr/bin/env python3
"""
Analyze struct codegen by comparing compiler output with expected assembly patterns.
"""

import sys
import subprocess
import tempfile
import os
import re
from pathlib import Path

def compile_to_obj(code):
    """Compile code to object file and return the path."""
    with tempfile.NamedTemporaryFile(mode='w', suffix='.c', delete=False) as f:
        f.write(code)
        src_file = f.name
    
    obj_file = src_file.replace('.c', '.o')
    result = subprocess.run(
        ['build/bin/zerocc', '-c', src_file, '-o', obj_file],
        capture_output=True, text=True, cwd='/home/ubuntu/dev/qnz_c_compiler'
    )
    
    os.unlink(src_file)
    
    if result.returncode != 0:
        return None, result.stderr
    
    return obj_file, None

def disassemble_obj(obj_file):
    """Disassemble object file and return assembly."""
    result = subprocess.run(
        ['objdump', '-d', obj_file],
        capture_output=True, text=True
    )
    return result.stdout

def analyze_struct_access(asm):
    """Analyze struct member access patterns in assembly."""
    print("\n" + "=" * 60)
    print("Struct Access Pattern Analysis")
    print("=" * 60)
    
    # Look for common patterns
    patterns = {
        'struct_load': re.findall(r'mov\s+(-?0x[0-9a-f]+)\(%[a-z0-9]+\),\s*%[a-z0-9]+', asm),
        'struct_store': re.findall(r'mov\s+%[a-z0-9]+,\s*(-?0x[0-9a-f]+)\(%[a-z0-9]+\)', asm),
        'lea_struct': re.findall(r'lea\s+0x([0-9a-f]+)\(%[a-z0-9]+\),\s*%[a-z0-9]+', asm),
    }
    
    print("\n1. Struct Member Loads (mov offset(reg), reg):")
    for pattern in patterns['struct_load'][:10]:
        offset = int(pattern, 16) if pattern.startswith('0x') else -int(pattern[1:], 16)
        print(f"   Offset: {offset} (0x{offset & 0xffffffff:x})")
    
    print("\n2. Struct Member Stores (mov reg, offset(reg)):")
    for pattern in patterns['struct_store'][:10]:
        offset = int(pattern, 16) if pattern.startswith('0x') else -int(pattern[1:], 16)
        print(f"   Offset: {offset} (0x{offset & 0xffffffff:x})")
    
    print("\n3. LEA instructions (address calculation):")
    for pattern in patterns['lea_struct'][:10]:
        offset = int(pattern, 16)
        print(f"   Offset: {offset} (0x{offset:x})")

def compare_with_gcc(code):
    """Compare ZeroCC output with GCC output."""
    print("\n" + "=" * 60)
    print("GCC Comparison")
    print("=" * 60)
    
    # Compile with ZeroCC
    zerocc_obj, zerocc_err = compile_to_obj(code)
    if zerocc_err:
        print(f"\nZeroCC compilation failed:")
        print(zerocc_err)
        return
    
    # Compile with GCC
    with tempfile.NamedTemporaryFile(mode='w', suffix='.c', delete=False) as f:
        f.write(code)
        src_file = f.name
    
    gcc_obj = src_file.replace('.c', '.gcc.o')
    subprocess.run(['gcc', '-c', '-O0', src_file, '-o', gcc_obj], 
                   capture_output=True, cwd='/home/ubuntu/dev/qnz_c_compiler')
    
    # Disassemble both
    zerocc_asm = disassemble_obj(zerocc_obj)
    gcc_asm = disassemble_obj(gcc_obj)
    
    # Extract function bodies
    zerocc_funcs = re.findall(r'<(\w+)>:(.*?)(?=<\w+>:|$)', zerocc_asm, re.DOTALL)
    gcc_funcs = re.findall(r'<(\w+)>:(.*?)(?=<\w+>:|$)', gcc_asm, re.DOTALL)
    
    print("\nZeroCC generated functions:")
    for name, body in zerocc_funcs:
        print(f"\n  {name}:")
        for line in body.strip().split('\n')[:10]:
            if line.strip():
                print(f"    {line.strip()}")
    
    print("\nGCC generated functions:")
    for name, body in gcc_funcs:
        print(f"\n  {name}:")
        for line in body.strip().split('\n')[:10]:
            if line.strip():
                print(f"    {line.strip()}")
    
    # Cleanup
    os.unlink(src_file)
    os.unlink(gcc_obj)

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <test_case_name>")
        print("\nAvailable test cases:")
        print("  1. struct_member_access")
        print("  2. struct_return_simple")
        print("  3. struct_as_param")
        print("  4. struct_return_and_param")
        sys.exit(1)
    
    test_name = sys.argv[1]
    
    test_cases = {
        'struct_member_access': """
typedef struct { int x; int y; } Point;
int main() {
    Point p;
    p.x = 1;
    p.y = 2;
    return p.x + p.y;
}
""",
        'struct_return_simple': """
typedef struct { int x; } Point;
Point create() {
    Point p;
    p.x = 42;
    return p;
}
int main() {
    Point p = create();
    return p.x;
}
""",
        'struct_as_param': """
typedef struct { int x; } Point;
int get_x(Point p) {
    return p.x;
}
int main() {
    Point p;
    p.x = 42;
    return get_x(p);
}
""",
        'struct_return_and_param': """
typedef struct { int x; } Point;
Point create() {
    Point p;
    p.x = 42;
    return p;
}
int get_x(Point p) {
    return p.x;
}
int main() {
    Point p = create();
    return get_x(p);
}
""",
    }
    
    if test_name not in test_cases:
        print(f"Unknown test case: {test_name}")
        sys.exit(1)
    
    code = test_cases[test_name]
    
    print(f"\nTest Case: {test_name}")
    print("=" * 60)
    print(code)
    
    obj_file, err = compile_to_obj(code)
    if err:
        print(f"\nCompilation failed:")
        print(err)
        return
    
    asm = disassemble_obj(obj_file)
    print("\nGenerated Assembly:")
    print(asm)
    
    analyze_struct_access(asm)
    compare_with_gcc(code)
    
    # Cleanup
    os.unlink(obj_file)

if __name__ == '__main__':
    main()
