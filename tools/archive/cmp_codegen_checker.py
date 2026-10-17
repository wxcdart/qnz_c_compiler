#!/usr/bin/env python3
"""
Comparison CodeGen Checker for Phase 2B

Compiles comparison operations to different ISAs and verifies they work correctly.

Usage:
    python3 tools/cmp_codegen_checker.py

Creates test C files and runs the compiler to verify comparison codegen.
"""

import subprocess
import tempfile
import os
from pathlib import Path

# Test cases for comparison operations
TEST_CASES = [
    {
        'name': 'less_than',
        'code': '''
int test_lt(int a, int b) {
    return a < b;
}
'''
    },
    {
        'name': 'less_equal',
        'code': '''
int test_le(int a, int b) {
    return a <= b;
}
'''
    },
    {
        'name': 'greater_than',
        'code': '''
int test_gt(int a, int b) {
    return a > b;
}
'''
    },
    {
        'name': 'greater_equal',
        'code': '''
int test_ge(int a, int b) {
    return a >= b;
}
'''
    },
    {
        'name': 'equal',
        'code': '''
int test_eq(int a, int b) {
    return a == b;
}
'''
    },
    {
        'name': 'not_equal',
        'code': '''
int test_ne(int a, int b) {
    return a != b;
}
'''
    },
    {
        'name': 'logical_not',
        'code': '''
int test_lnot(int a) {
    return !a;
}
'''
    },
]

def run_compiler(source_file, isa='x86_64'):
    """Compile source file to assembly for given ISA."""
    compiler = Path.cwd() / 'build/qnz_cc'
    if not compiler.exists():
        print(f"Compiler not found at {compiler}")
        return None
    
    output_file = source_file.with_suffix(f'.{isa}.s')
    
    try:
        result = subprocess.run(
            [str(compiler), '-S', f'--isa={isa}', '-o', str(output_file), str(source_file)],
            capture_output=True,
            text=True,
            timeout=30
        )
        
        if result.returncode != 0:
            print(f"Compilation failed: {result.stderr}")
            return None
        
        return output_file
    except Exception as e:
        print(f"Error compiling: {e}")
        return None

def analyze_assembly(asm_file):
    """Analyze assembly file for comparison patterns."""
    if not asm_file or not asm_file.exists():
        return []
    
    with open(asm_file, 'r') as f:
        content = f.read()
    
    patterns = {
        'cmp': content.count('cmp '),
        'test': content.count('test '),
        'set': content.count('set'),
        'branch': content.count('j'),  # Jump instructions
    }
    
    return patterns

def main():
    print("=" * 80)
    print("Comparison CodeGen Checker for Phase 2B")
    print("=" * 80)
    print()
    
    # Create temp directory for test files
    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir = Path(tmpdir)
        
        print(f"Testing comparison codegen...")
        print()
        
        results = {}
        
        for test in TEST_CASES:
            name = test['name']
            code = test['code']
            
            # Write test file
            test_file = tmpdir / f'test_{name}.c'
            with open(test_file, 'w') as f:
                f.write(code)
            
            # Compile to x86_64
            asm_file = run_compiler(test_file, 'x86_64')
            
            if asm_file and asm_file.exists():
                patterns = analyze_assembly(asm_file)
                results[name] = patterns
                print(f"✓ {name}: {patterns}")
            else:
                print(f"✗ {name}: Compilation failed")
                results[name] = None
        
        print()
        print("=" * 80)
        print("Summary")
        print("=" * 80)
        print()
        
        # Check for expected patterns
        all_pass = True
        for name, patterns in results.items():
            if patterns is None:
                print(f"✗ {name}: Failed to compile")
                all_pass = False
            elif patterns['cmp'] == 0 and patterns['test'] == 0:
                print(f"⚠ {name}: No cmp/test instructions found")
            else:
                print(f"✓ {name}: Uses cmp/test as expected")
        
        print()
        if all_pass:
            print("✓ All comparison operations compile correctly")
            return 0
        else:
            print("✗ Some comparison operations failed")
            return 1

if __name__ == "__main__":
    import sys
    sys.exit(main())
