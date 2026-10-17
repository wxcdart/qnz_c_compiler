#!/usr/bin/env python3
"""
Debug tool for struct handling in ZeroCC.

This tool helps diagnose struct-related compilation issues by:
1. Analyzing struct definitions and usage patterns
2. Generating minimal test cases to isolate bugs
3. Comparing compiler output with expected behavior
"""

import sys
import re
import subprocess
import tempfile
import os
from pathlib import Path

def create_test_case(name, code, expected_pass=True):
    """Create a test case file and attempt compilation."""
    test_file = f"/tmp/struct_test_{name}.c"
    with open(test_file, 'w') as f:
        f.write(code)
    return test_file

def compile_test(test_file, zerocc_path, verbose=False):
    """Compile a test file and return results."""
    obj_file = test_file.replace('.c', '.o')
    exe_file = test_file.replace('.c', '.exe')
    
    cmd = [zerocc_path, '-c', test_file, '-o', obj_file]
    if verbose:
        cmd.append('-v')
    
    result = subprocess.run(cmd, capture_output=True, text=True)
    
    if result.returncode != 0:
        return {'pass': False, 'error': result.stderr, 'stage': 'compile'}
    
    # Try to link
    link_result = subprocess.run(['gcc', '-no-pie', '-o', exe_file, obj_file],
                                  capture_output=True, text=True)
    if link_result.returncode != 0:
        return {'pass': False, 'error': link_result.stderr, 'stage': 'link'}
    
    # Try to run
    run_result = subprocess.run([exe_file], capture_output=True, text=True)
    if run_result.returncode != 0:
        return {'pass': False, 'error': f'Runtime error: {run_result.stderr}', 
                'stage': 'run', 'exit_code': run_result.returncode}
    
    return {'pass': True, 'stage': 'all'}

def analyze_struct_patterns(code):
    """Analyze struct usage patterns in code."""
    patterns = {
        'struct_defs': re.findall(r'typedef\s+struct\s*\{[^}]+\}\s*(\w+)', code, re.DOTALL),
        'struct_vars': re.findall(r'(\w+)\s+(\w+)\s*;', code),
        'struct_members': re.findall(r'(\w+)\.(\w+)', code),
        'struct_returns': re.findall(r'(\w+)\s+(\w+)\s*\([^)]*\)\s*\{', code),
        'struct_args': re.findall(r'(\w+)\s*\(([^)]+)\)', code),
    }
    return patterns

def run_struct_debugger(zerocc_path, test_file=None):
    """Run the struct debugging analysis."""
    if test_file:
        with open(test_file, 'r') as f:
            code = f.read()
    else:
        code = """
typedef struct {
    int x;
    int y;
} Point;

Point create_point(int x, int y) {
    Point p;
    p.x = x;
    p.y = y;
    return p;
}

int get_x(Point p) {
    return p.x;
}

int main() {
    Point p = create_point(1, 2);
    return get_x(p);
}
"""
    
    print("=" * 60)
    print("Struct Usage Analysis")
    print("=" * 60)
    
    patterns = analyze_struct_patterns(code)
    
    print("\n1. Struct Definitions:")
    for s in patterns['struct_defs']:
        print(f"   - {s}")
    
    print("\n2. Struct Member Access:")
    for obj, member in patterns['struct_members']:
        print(f"   - {obj}.{member}")
    
    print("\n3. Function Signatures:")
    for ret_type, func_name in patterns['struct_returns']:
        print(f"   - {ret_type} {func_name}(...)")
    
    print("\n" + "=" * 60)
    print("Test Case Generation")
    print("=" * 60)
    
    # Test cases to isolate the bug
    test_cases = [
        ("struct_def_only", """
typedef struct { int x; int y; } Point;
int main() { return 0; }
""", True),
        
        ("struct_member_access", """
typedef struct { int x; int y; } Point;
int main() {
    Point p;
    p.x = 1;
    p.y = 2;
    return p.x + p.y;
}
""", True),
        
        ("struct_return_simple", """
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
""", True),
        
        ("struct_as_param", """
typedef struct { int x; } Point;
int get_x(Point p) {
    return p.x;
}
int main() {
    Point p;
    p.x = 42;
    return get_x(p);
}
""", True),
        
        ("struct_return_and_param", """
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
""", True),
        
        ("nested_struct", """
typedef struct { int x; int y; } Point;
typedef struct { Point origin; Point dest; } Line;
int main() {
    Line l;
    l.origin.x = 0;
    l.origin.y = 0;
    l.dest.x = 3;
    l.dest.y = 4;
    return l.dest.x + l.dest.y;
}
""", True),
        
        ("struct_array", """
typedef struct { int x; int y; } Point;
int main() {
    Point points[2];
    points[0].x = 1;
    points[1].x = 2;
    return points[0].x + points[1].x;
}
""", True),
        
        ("struct_pointer", """
typedef struct { int x; int y; } Point;
int get_x(Point *p) {
    return p->x;
}
int main() {
    Point p;
    p.x = 42;
    return get_x(&p);
}
""", True),
    ]
    
    results = []
    for name, test_code, expected in test_cases:
        test_file = create_test_case(name, test_code)
        result = compile_test(test_file, zerocc_path)
        results.append((name, result))
        
        status = "✅" if result['pass'] else "❌"
        print(f"\n{status} {name}")
        if not result['pass']:
            print(f"   Stage: {result['stage']}")
            error_lines = result['error'].strip().split('\n')
            for line in error_lines[:3]:
                print(f"   {line}")
    
    print("\n" + "=" * 60)
    print("Summary")
    print("=" * 60)
    
    passed = sum(1 for _, r in results if r['pass'])
    total = len(results)
    print(f"Passed: {passed}/{total}")
    
    if passed < total:
        print("\nFailed tests (likely bug locations):")
        for name, r in results:
            if not r['pass']:
                print(f"  - {name} (stage: {r['stage']})")
    
    return results

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <zerocc_path> [test_file]")
        print("\nThis tool helps diagnose struct-related compilation issues.")
        print("It generates minimal test cases to isolate the bug.")
        sys.exit(1)
    
    zerocc_path = sys.argv[1]
    test_file = sys.argv[2] if len(sys.argv) > 2 else None
    
    if not os.path.exists(zerocc_path):
        print(f"Error: zerocc not found at {zerocc_path}")
        sys.exit(1)
    
    run_struct_debugger(zerocc_path, test_file)

if __name__ == '__main__':
    main()
