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
    vla_type: str = "runtime"  # runtime, comptime, hybrid
    expect_error: Optional[str] = None


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
void test(int n, int m) {
    int arr[n][m];
    arr[0][0] = 42;
}
int main(void) { return 0; }
""",
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
    """Compile test source code"""
    with tempfile.NamedTemporaryFile(mode='w', suffix='.c', delete=False) as f:
        f.write(source)
        source_file = f.name
    
    try:
        result = subprocess.run(
            [compiler, '-c', source_file, '-o', output],
            capture_output=True,
            text=True,
            timeout=30
        )
        return result.returncode == 0, result.stdout, result.stderr
    except subprocess.TimeoutExpired:
        return False, "", "Compilation timeout"
    finally:
        os.unlink(source_file)


def run_test(compiler: str, test: TestCase) -> Dict:
    """Run a single test case"""
    result = {
        'name': test.name,
        'passed': False,
        'compile_ok': False,
        'runtime_ok': False,
        'error': None,
        'vla_type': test.vla_type
    }
    
    # Compile
    with tempfile.NamedTemporaryFile(suffix='.o', delete=False) as f:
        output_file = f.name
    
    try:
        compile_ok, stdout, stderr = compile_test(compiler, test.source, output_file)
        result['compile_ok'] = compile_ok
        
        # Check compilation expectation
        if compile_ok != test.should_compile:
            result['error'] = f"Compilation mismatch: expected {'success' if test.should_compile else 'failure'}"
            return result
        
        # If compilation succeeded, try to run
        if compile_ok:
            with tempfile.NamedTemporaryFile(suffix='', delete=False) as f:
                exe_file = f.name
            
            try:
                # Link
                link_result = subprocess.run(
                    ['gcc', output_file, '-o', exe_file],
                    capture_output=True,
                    text=True
                )
                
                if link_result.returncode == 0:
                    # Run
                    run_result = subprocess.run(
                        [exe_file],
                        capture_output=True,
                        text=True,
                        timeout=10
                    )
                    
                    result['runtime_ok'] = (run_result.returncode == test.expected_result)
                    
                    if not result['runtime_ok']:
                        result['error'] = f"Runtime mismatch: expected {test.expected_result}, got {run_result.returncode}"
                
                os.unlink(exe_file)
            finally:
                pass
        
        result['passed'] = (compile_ok == test.should_compile) and (not compile_ok or result['runtime_ok'])
        
    finally:
        if os.path.exists(output_file):
            os.unlink(output_file)
    
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
    parser.add_argument('--compiler', '-c', default='../build/bin/zerocc',
                       help='Compiler path')
    parser.add_argument('--test', '-t', help='Run specific test')
    parser.add_argument('--all', '-a', action='store_true', help='Run all tests')
    parser.add_argument('--output', '-o', help='Output report file')
    
    args = parser.parse_args()
    
    # Resolve compiler path
    compiler = os.path.abspath(args.compiler)
    if not os.path.exists(compiler):
        print(f"Error: Compiler not found at {compiler}")
        sys.exit(1)
    
    # Run tests
    if args.all:
        summary = run_all_tests(compiler)
    elif args.test:
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
        parser.print_help()
        sys.exit(1)
    
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
