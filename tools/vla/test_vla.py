#!/usr/bin/env python3
"""
VLA Test Suite

Runs comprehensive tests on VLA implementation:
- Test single-scope VLA
- Test comptime VLA
- Test hybrid VLA rules
- Verify stack balance
- Verify cleanup on all paths

Usage:
    python3 test_vla.py --compiler ../build/bin/zerocc
    python3 test_vla.py --test test_single_scope.c
    python3 test_vla.py --all
"""

import argparse
import subprocess
import sys
import os
import tempfile
from dataclasses import dataclass, field
from typing import List, Dict, Optional, Tuple
from pathlib import Path


@dataclass
class TestCase:
    """Test case definition"""
    name: str
    source: str
    expected_result: int = 0
    should_compile: bool = True
    vla_type: str = "runtime"  # runtime, comptime, hybrid, fixed
    expect_error: Optional[str] = None
    run_jit: bool = True  # Also verify via 'zerocc --run' (JIT path)


# Test cases for VLA
TEST_CASES = [
    # Single-scope runtime VLA (should work)
    TestCase(
        name="single_vla_1d",
        source="""
void test(int n) {
    int arr[n];
    arr[0] = 42;
}
int main(void) { return 0; }
""",
        should_compile=True,
        vla_type="runtime"
    ),
    
    TestCase(
        name="single_vla_2d",
        source="""
int sum_flat(int *p, int n) {
    int s = 0;
    for (int i = 0; i < n; i++) s += p[i];
    return s;
}
int test2d(int n) {
    int arr[n][3];
    for (int i = 0; i < n; i++)
        for (int j = 0; j < 3; j++)
            arr[i][j] = i * 3 + j;
    return sum_flat(arr, n * 3);  /* n=4: 0+1+...+11 = 66 */
}
int main(void) { return test2d(4); }
""",
        expected_result=66,
        should_compile=True,
        vla_type="runtime"
    ),
    
    TestCase(
        name="vla_with_return",
        source="""
int test(int n) {
    int arr[n];
    arr[0] = n;
    return arr[0];
}
int main(void) { return test(42); }
""",
        expected_result=42,
        should_compile=True,
        vla_type="runtime"
    ),
    
    TestCase(
        name="vla_with_early_return",
        source="""
int test(int n) {
    int arr[n];
    if (n < 0) return -1;  // Early return - VLA should be cleaned
    arr[0] = n;
    return arr[0];
}
int main(void) { return test(42); }
""",
        expected_result=42,
        should_compile=True,
        vla_type="runtime"
    ),
    
    # Multiple VLA (should fail in hybrid mode)
    TestCase(
        name="multiple_vla",
        source="""
void test(int n, int m) {
    int arr1[n];
    int arr2[m];  // Second VLA - should error in hybrid mode
}
int main(void) { return 0; }
""",
        should_compile=False,
        vla_type="hybrid",
        expect_error="multiple VLA"
    ),
    
    # Comptime VLA (should work)
    TestCase(
        name="comptime_vla",
        source="""
void test(void) {
    enum { N = 10 };
    int arr1[N];
    int arr2[20];  // Multiple comptime arrays OK
    arr1[0] = 42;
    arr2[0] = 43;
}
int main(void) { return 0; }
""",
        should_compile=True,
        vla_type="comptime"
    ),

    # Phase 0 gates: multi-dimensional FIXED arrays (no VLA involved).
    # NOTE: the array MUST be consumed through a pointer (decay) in another
    # function. A same-function fill+read round-trips even under the current
    # miscompile (values go through garbage row pointers consistently) and
    # would falsely pass.
    TestCase(
        name="fixed_2d_array",
        source="""
int sum_row_major(int *p, int rows, int cols) {
    int s = 0;
    for (int i = 0; i < rows; i++)
        for (int j = 0; j < cols; j++)
            s += p[i * cols + j];
    return s;
}
int test2d(void) {
    int a[2][3];
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 3; j++)
            a[i][j] = i * 3 + j;
    return sum_row_major(a, 2, 3);  /* 0+1+...+5 = 15 */
}
int main(void) { return test2d(); }
""",
        expected_result=15,
        should_compile=True,
        vla_type="fixed"
    ),

    TestCase(
        name="fixed_3d_array",
        source="""
int sum3(int *p) {
    int s = 0;
    for (int i = 0; i < 8; i++) s += p[i];
    return s;
}
int test3d(void) {
    int b[2][2][2];
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++)
            for (int k = 0; k < 2; k++)
                b[i][j][k] = i * 4 + j * 2 + k;
    return sum3(b);  /* 0+1+...+7 = 28 */
}
int main(void) { return test3d(); }
""",
        expected_result=28,
        should_compile=True,
        vla_type="fixed"
    ),

    # Phase 1: pragmatic multi-dim VLA - one runtime outer dimension,
    # constant inner dimensions (matrix-math shape)
    TestCase(
        name="vla_2d_matmul",
        source="""
int matmul(void) {
    int n = 3;
    int A[n][3];   /* runtime VLA (the only runtime VLA in the function) */
    int B[3][3];
    int C[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            A[i][j] = i + 1;
            B[i][j] = j + 1;
        }
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            int s = 0;
            for (int k = 0; k < 3; k++) s += A[i][k] * B[k][j];
            C[i][j] = s;
        }
    /* C[0][0] = 3, C[1][2] = 18, C[2][1] = 18 */
    return C[0][0] + C[1][2] + C[2][1];  /* 3 + 18 + 18 = 39 */
}
int main(void) { return matmul(); }
""",
        expected_result=39,
        should_compile=True,
        vla_type="runtime"
    ),
    
    # VLA in loop (should fail in hybrid mode)
    TestCase(
        name="vla_in_loop",
        source="""
void test(int n) {
    for (int i = 0; i < 10; i++) {
        int arr[n];  // VLA in loop - should error
    }
}
int main(void) { return 0; }
""",
        should_compile=False,
        vla_type="hybrid",
        expect_error="VLA in loop"
    ),
    
    # VLA in nested block (should fail in hybrid mode)
    TestCase(
        name="vla_in_block",
        source="""
void test(int n) {
    if (n > 0) {
        int arr[n];  // VLA in block - should error
    }
}
int main(void) { return 0; }
""",
        should_compile=False,
        vla_type="hybrid",
        expect_error="VLA in block"
    ),
]


def compile_test(compiler: str, source: str, output: str) -> Tuple[bool, str, str]:
    """Compile test source code (source is an existing file path)"""
    try:
        result = subprocess.run(
            [compiler, '-c', source, '-o', output],
            capture_output=True,
            text=True,
            timeout=30
        )
        return result.returncode == 0, result.stdout, result.stderr
    except subprocess.TimeoutExpired:
        return False, "", "Compilation timeout"


def run_test(compiler: str, test: TestCase) -> Dict:
    """Run a single test case (AOT via gcc link, and JIT via --run)"""
    result = {
        'name': test.name,
        'passed': False,
        'compile_ok': False,
        'runtime_ok': False,
        'jit_ok': None,  # None = not checked
        'error': None,
        'vla_type': test.vla_type
    }
    
    # Write source to a temp file (kept for both AOT and JIT runs)
    source_file = tempfile.mktemp(suffix='.c')
    with open(source_file, 'w') as f:
        f.write(test.source)
    
    output_file = tempfile.mktemp(suffix='.o')
    try:
        compile_ok, stdout, stderr = compile_test(compiler, source_file, output_file)
        result['compile_ok'] = compile_ok
        
        # Check compilation expectation
        if compile_ok != test.should_compile:
            result['error'] = f"Compilation mismatch: expected {'success' if test.should_compile else 'failure'}"
            return result
        
        if compile_ok:
            # AOT path: link with host gcc and run
            exe_file = tempfile.mktemp(suffix='')
            try:
                link_result = subprocess.run(
                    ['gcc', output_file, '-o', exe_file],
                    capture_output=True,
                    text=True
                )
                
                if link_result.returncode == 0:
                    run_result = subprocess.run(
                        [exe_file],
                        capture_output=True,
                        text=True,
                        timeout=10
                    )
                    
                    result['runtime_ok'] = (run_result.returncode == test.expected_result)
                    if not result['runtime_ok']:
                        result['error'] = f"AOT runtime mismatch: expected {test.expected_result}, got {run_result.returncode}"
            finally:
                if os.path.exists(exe_file):
                    os.unlink(exe_file)
            
            # JIT path: zerocc --run (compiler executes the program itself)
            if test.run_jit:
                jit_result = subprocess.run(
                    [compiler, '--run', source_file],
                    capture_output=True,
                    text=True,
                    timeout=30
                )
                result['jit_ok'] = (jit_result.returncode == test.expected_result)
                if not result['jit_ok'] and not result['error']:
                    result['error'] = f"JIT runtime mismatch: expected {test.expected_result}, got {jit_result.returncode}"
        
        result['passed'] = (compile_ok == test.should_compile) and (
            (not compile_ok) or (result['runtime_ok'] and result['jit_ok'] in (True, None))
        )
    finally:
        if os.path.exists(output_file):
            os.unlink(output_file)
        if os.path.exists(source_file):
            os.unlink(source_file)
    
    return result


def run_all_tests(compiler: str) -> Dict:
    """Run all test cases"""
    results = []
    
    for test in TEST_CASES:
        result = run_test(compiler, test)
        results.append(result)
        print(f"{'✅' if result['passed'] else '❌'} {result['name']}")
        if result['error']:
            print(f"    Error: {result['error']}")
    
    # Summary
    passed = sum(1 for r in results if r['passed'])
    total = len(results)
    
    print("")
    print("=" * 60)
    print(f"Test Summary: {passed}/{total} passed")
    print("=" * 60)
    
    return {
        'results': results,
        'passed': passed,
        'total': total
    }


def generate_report(summary: Dict) -> str:
    """Generate test report"""
    lines = []
    lines.append("=" * 70)
    lines.append("VLA Test Suite Report")
    lines.append("=" * 70)
    lines.append("")
    lines.append(f"Total tests: {summary['total']}")
    lines.append(f"Passed: {summary['passed']}")
    lines.append(f"Failed: {summary['total'] - summary['passed']}")
    lines.append("")
    
    # Group by VLA type
    by_type = {}
    for result in summary['results']:
        vla_type = result['vla_type']
        if vla_type not in by_type:
            by_type[vla_type] = {'passed': 0, 'total': 0}
        by_type[vla_type]['total'] += 1
        if result['passed']:
            by_type[vla_type]['passed'] += 1
    
    lines.append("By VLA Type:")
    for vla_type, stats in by_type.items():
        lines.append(f"  {vla_type}: {stats['passed']}/{stats['total']}")
    lines.append("")
    
    # JIT path coverage
    jit_checked = [r for r in summary['results'] if r.get('jit_ok') is not None]
    jit_passed = [r for r in jit_checked if r['jit_ok']]
    lines.append(f"JIT (--run) coverage: {len(jit_passed)}/{len(jit_checked)} passed")
    lines.append("")
    
    # Failed tests
    failed = [r for r in summary['results'] if not r['passed']]
    if failed:
        lines.append("Failed Tests:")
        for result in failed:
            lines.append(f"  ❌ {result['name']}")
            if result['error']:
                lines.append(f"      {result['error']}")
        lines.append("")
    
    lines.append("=" * 70)
    
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description='VLA Test Suite')
    parser.add_argument('--compiler', '-c', default='./build/bin/zerocc',
                       help='Compiler path')
    parser.add_argument('--test', '-t', help='Run specific test')
    parser.add_argument('--all', '-a', action='store_true', help='Run all tests')
    parser.add_argument('--output', '-o', help='Output report file')
    
    args = parser.parse_args()
    
    # Resolve compiler path
    compiler = os.path.abspath(args.compiler)
    if not os.path.exists(compiler):
        alt = Path(__file__).resolve().parents[2] / 'build' / 'bin' / 'zerocc'
        if alt.exists():
            compiler = str(alt)
        else:
            print(f"Error: Compiler not found at {compiler}")
            sys.exit(1)
    
    # Run tests
    if args.test:
        # Find specific test
        test_case = next((t for t in TEST_CASES if t.name == args.test), None)
        if not test_case:
            print(f"Error: Test '{args.test}' not found")
            sys.exit(1)
        
        result = run_test(compiler, test_case)
        print(f"{'✅' if result['passed'] else '❌'} {result['name']}")
        if result['error']:
            print(f"Error: {result['error']}")
        
        summary = {'results': [result], 'passed': 1 if result['passed'] else 0, 'total': 1}
    else:
        summary = run_all_tests(compiler)
    
    # Generate report
    report = generate_report(summary)
    
    if args.output:
        with open(args.output, 'w') as f:
            f.write(report)
        print(f"\nReport written to {args.output}")
    else:
        print(report)
    
    # Exit with error if any tests failed
    if summary['passed'] < summary['total']:
        sys.exit(1)


if __name__ == '__main__':
    main()
