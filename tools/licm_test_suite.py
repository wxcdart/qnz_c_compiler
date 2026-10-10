#!/usr/bin/env python3
"""
LICM Test Suite Generator

Generates test cases and IR dumps for LICM verification:
1. Creates test C files with known invariant patterns
2. Dumps IR before and after optimization
3. Runs verifier to check hoisting correctness
4. Benchmarks performance improvement

Usage:
    python3 tools/licm_test_suite.py --generate
    python3 tools/licm_test_suite.py --verify --test simple_invariant
    python3 tools/licm_test_suite.py --benchmark --iterations 100000000
"""

import argparse
import subprocess
import sys
import os
import shutil

TEST_CASES = {
    'simple_invariant': '''
int test(int *arr, int n, int factor) {
    int result = 0;
    for (int i = 0; i < n; i++) {
        result += arr[i] * factor;
    }
    return result;
}
''',
    'load_invariant': '''
int test(int *arr, int *mult, int n) {
    int result = 0;
    int m = *mult;
    for (int i = 0; i < n; i++) {
        result += arr[i] * m;
    }
    return result;
}
''',
    'complex_invariant': '''
int test(int *arr, int base, int n) {
    int result = 0;
    int t1 = base + 10;
    int t2 = t1 * 2;
    int factor = t2 - 5;
    for (int i = 0; i < n; i++) {
        result += arr[i] * factor;
    }
    return result;
}
''',
    'multiple_invariants': '''
int test(int *arr, int n, int a, int b, int c) {
    int result = 0;
    int x = a + b;
    int y = x * c;
    for (int i = 0; i < n; i++) {
        result += arr[i] + y;
    }
    return result;
}
''',
    'nested_loops': '''
int test(int **mat, int rows, int cols, int factor) {
    int result = 0;
    for (int i = 0; i < rows; i++) {
        int row_offset = i * cols;  // Loop-invariant in outer loop
        for (int j = 0; j < cols; j++) {
            result += mat[i][j] * factor;
        }
    }
    return result;
}
'''
}

def generate_test_case(name: str):
    """Generate a test C file"""
    if name not in TEST_CASES:
        print(f"Unknown test case: {name}")
        print(f"Available: {', '.join(TEST_CASES.keys())}")
        return False
    
    test_dir = 'test/licm'
    os.makedirs(test_dir, exist_ok=True)
    
    test_file = f"{test_dir}/{name}.c"
    with open(test_file, 'w') as f:
        f.write(f"/* LICM Test: {name} */\n")
        f.write(TEST_CASES[name])
    
    print(f"Generated: {test_file}")
    return True

def dump_ir(test_file: str, output_file: str, optimize: bool = True):
    """Dump IR for a test file"""
    cmd = ['./build/bin/zerocc']
    if optimize:
        cmd.append('-O1')
    cmd.extend(['-dump-ir', '-c', test_file, '-o', '/tmp/test.o'])
    
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        # IR dump goes to stderr
        with open(output_file, 'w') as f:
            f.write(result.stderr)
        return result.returncode == 0
    except Exception as e:
        print(f"Error: {e}")
        return False

def run_verification(test_name: str):
    """Run LICM verification for a test case"""
    test_file = f"test/licm/{test_name}.c"
    if not os.path.exists(test_file):
        print(f"Test file not found: {test_file}")
        print("Run with --generate first")
        return False
    
    ir_before = f"/tmp/licm_{test_name}_before.txt"
    ir_after = f"/tmp/licm_{test_name}_after.txt"
    
    # Dump IR before optimization (-O0)
    print(f"\nDumping IR before optimization (-O0)...")
    if not dump_ir(test_file, ir_before, optimize=False):
        print("Failed to dump IR before optimization")
        return False
    
    # Dump IR after optimization (-O1 with LICM)
    print(f"Dumping IR after optimization (-O1)...")
    if not dump_ir(test_file, ir_after, optimize=True):
        print("Failed to dump IR after optimization")
        return False
    
    # Run verifier
    print(f"\nRunning LICM verifier...")
    verifier = './tools/licm_verifier.py'
    result = subprocess.run(
        ['python3', verifier, '--dump-before', ir_before, '--dump-after', ir_after],
        capture_output=True,
        text=True
    )
    
    print(result.stdout)
    if result.stderr:
        print(f"Verifier errors: {result.stderr}")
    
    return result.returncode == 0

def run_benchmark(iterations: int = 100000000):
    """Run performance benchmark"""
    test_file = 'test/bench/licm_invariant_test.c'
    
    if not os.path.exists(test_file):
        print(f"Benchmark file not found: {test_file}")
        return False
    
    print(f"\nRunning LICM benchmark ({iterations:,} iterations)...")
    
    # Compile with ZeroCC
    print("Compiling with ZeroCC -O1...")
    result = subprocess.run(
        ['./build/bin/zerocc', '-O1', '-c', test_file, '-o', '/tmp/licm_bench.o'],
        capture_output=True,
        text=True,
        timeout=30
    )
    
    if result.returncode != 0:
        print(f"Compilation failed: {result.stderr}")
        return False
    
    print("Benchmark complete (see tools/licm_verifier.py for timing)")
    return True

def main():
    parser = argparse.ArgumentParser(description='LICM Test Suite')
    parser.add_argument('--generate', nargs='*', help='Generate test cases')
    parser.add_argument('--verify', help='Verify a specific test case')
    parser.add_argument('--benchmark', action='store_true', help='Run benchmark')
    parser.add_argument('--iterations', type=int, default=100000000, help='Benchmark iterations')
    parser.add_argument('--all', action='store_true', help='Run all tests')
    
    args = parser.parse_args()
    
    if args.generate is not None:
        # Generate specific test cases or all
        cases = args.generate if args.generate else list(TEST_CASES.keys())
        for case in cases:
            generate_test_case(case)
    
    elif args.verify:
        run_verification(args.verify)
    
    elif args.benchmark:
        run_benchmark(args.iterations)
    
    elif args.all:
        print("=== LICM Test Suite ===\n")
        
        # Generate all test cases
        print("Generating test cases...")
        for case in TEST_CASES.keys():
            generate_test_case(case)
        
        # Verify all test cases
        print("\nVerifying test cases...")
        passed = 0
        failed = 0
        for case in TEST_CASES.keys():
            print(f"\n{'='*60}")
            print(f"Testing: {case}")
            print(f"{'='*60}")
            if run_verification(case):
                passed += 1
            else:
                failed += 1
        
        print(f"\n{'='*60}")
        print(f"Results: {passed} passed, {failed} failed")
        print(f"{'='*60}")
    
    else:
        parser.print_help()

if __name__ == '__main__':
    main()
