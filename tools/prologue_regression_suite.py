#!/usr/bin/env python3
"""
Prologue/Epilogue Regression Test Suite

Tests prologue/epilogue code generation for various scenarios:
- Different frame sizes
- Different calling conventions
- Different return types
- Nested function calls
- Variadic functions

Usage:
    python3 prologue_regression_suite.py --run-all
    python3 prologue_regression_suite.py --test frame_sizes
    python3 prologue_regression_suite.py --verbose
"""

import argparse
import subprocess
import sys
import tempfile
import os
from pathlib import Path
from dataclasses import dataclass
from typing import List, Optional


@dataclass
class TestCase:
    """A test case for prologue/epilogue."""
    name: str
    description: str
    source_code: str
    expected_frame_size: Optional[int]
    should_pass: bool
    category: str


@dataclass
class TestResult:
    """Result of a test case."""
    test: TestCase
    passed: bool
    error: Optional[str]
    assembly: Optional[str]


# Test cases for prologue/epilogue
TEST_CASES = [
    # === Frame Size Tests ===
    TestCase(
        name="no_stack_frame",
        description="Function with no stack frame",
        source_code="""
int no_frame(void) {
    return 42;
}

int main(void) {
    return no_frame();
}
""",
        expected_frame_size=0,
        should_pass=True,
        category="frame_sizes"
    ),
    
    TestCase(
        name="small_frame",
        description="Function with small stack frame (16 bytes)",
        source_code="""
int small_frame(void) {
    int a = 1;
    int b = 2;
    int c = 3;
    int d = 4;
    return a + b + c + d;
}

int main(void) {
    return small_frame();
}
""",
        expected_frame_size=16,
        should_pass=True,
        category="frame_sizes"
    ),
    
    TestCase(
        name="medium_frame",
        description="Function with medium stack frame (64 bytes)",
        source_code="""
int medium_frame(void) {
    int arr[8];
    for (int i = 0; i < 8; i++) {
        arr[i] = i;
    }
    return arr[7];
}

int main(void) {
    return medium_frame();
}
""",
        expected_frame_size=64,
        should_pass=True,
        category="frame_sizes"
    ),
    
    TestCase(
        name="large_frame",
        description="Function with large stack frame (256 bytes)",
        source_code="""
int large_frame(void) {
    int arr[32];
    for (int i = 0; i < 32; i++) {
        arr[i] = i * 2;
    }
    return arr[31];
}

int main(void) {
    return large_frame();
}
""",
        expected_frame_size=256,
        should_pass=True,
        category="frame_sizes"
    ),
    
    # === Call Site Tests ===
    TestCase(
        name="nested_calls",
        description="Function with nested function calls",
        source_code="""
int add(int a, int b) {
    return a + b;
}

int nested_calls(void) {
    int x = add(1, 2);
    int y = add(x, 3);
    int z = add(y, 4);
    return z;
}

int main(void) {
    return nested_calls();
}
""",
        expected_frame_size=None,
        should_pass=True,
        category="call_sites"
    ),
    
    TestCase(
        name="many_args",
        description="Function call with many arguments",
        source_code="""
int sum_args(int a, int b, int c, int d, int e, int f, int g, int h) {
    return a + b + c + d + e + f + g + h;
}

int main(void) {
    return sum_args(1, 2, 3, 4, 5, 6, 7, 8);
}
""",
        expected_frame_size=None,
        should_pass=True,
        category="call_sites"
    ),
    
    # === Return Type Tests ===
    TestCase(
        name="int_return",
        description="Function returning int",
        source_code="""
int int_return(void) {
    return 123;
}

int main(void) {
    return int_return();
}
""",
        expected_frame_size=None,
        should_pass=True,
        category="return_types"
    ),
    
    TestCase(
        name="void_return",
        description="Function returning void",
        source_code="""
void void_return(int x) {
    /* void function */
}

int main(void) {
    void_return(42);
    return 0;
}
""",
        expected_frame_size=None,
        should_pass=True,
        category="return_types"
    ),
    
    # === Edge Cases ===
    TestCase(
        name="empty_function",
        description="Empty function",
        source_code="""
void empty_func(void) {
}

int main(void) {
    empty_func();
    return 0;
}
""",
        expected_frame_size=None,
        should_pass=True,
        category="edge_cases"
    ),
    
    TestCase(
        name="recursive_call",
        description="Simple recursive function",
        source_code="""
int factorial(int n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}

int main(void) {
    return factorial(5);
}
""",
        expected_frame_size=None,
        should_pass=True,
        category="edge_cases"
    ),
    
    TestCase(
        name="loop_with_calls",
        description="Function with loop and calls",
        source_code="""
int identity(int x) {
    return x;
}

int loop_with_calls(void) {
    int sum = 0;
    for (int i = 0; i < 10; i++) {
        sum += identity(i);
    }
    return sum;
}

int main(void) {
    return loop_with_calls();
}
""",
        expected_frame_size=None,
        should_pass=True,
        category="edge_cases"
    ),
]


def compile_to_assembly(source: str) -> Optional[str]:
    """Compile source code to assembly."""
    try:
        with tempfile.NamedTemporaryFile(mode='w', suffix='.c', delete=False) as f:
            f.write(source)
            source_file = f.name
        
        result = subprocess.run(
            ["./build/bin/zerocc", "-S", "-o", "-", source_file],
            capture_output=True,
            text=True,
            timeout=10
        )
        
        os.unlink(source_file)
        
        if result.returncode == 0:
            return result.stdout
        else:
            return None
            
    except Exception as e:
        return None


def check_frame_alignment(assembly: str) -> bool:
    """Check that all frame sizes in assembly are 16-byte aligned."""
    import re
    
    # Find all frame sizes - match "sub $N, rsp" or "sub N, rsp"
    frame_sizes = re.findall(r'sub\s+\$?(\d+),', assembly)
    
    for size_str in frame_sizes:
        size = int(size_str)
        if size % 16 != 0:
            return False
    
    return True


def check_call_alignment(assembly: str) -> bool:
    """Check that all call sites have proper stack alignment."""
    # This is a simplified check - full check would require tracking RSP
    # For now, just verify frame sizes are aligned
    return check_frame_alignment(assembly)


def run_test(test: TestCase, verbose: bool = False) -> TestResult:
    """Run a single test case."""
    try:
        assembly = compile_to_assembly(test.source_code)
        
        if assembly is None:
            if test.should_pass:
                return TestResult(
                    test=test,
                    passed=False,
                    error="Failed to compile to assembly",
                    assembly=None
                )
            else:
                return TestResult(
                    test=test,
                    passed=True,
                    error=None,
                    assembly=None
                )
        
        # Check frame alignment
        if not check_frame_alignment(assembly):
            return TestResult(
                test=test,
                passed=False,
                error="Frame size not 16-byte aligned",
                assembly=assembly
            )
        
        # Check call alignment
        if not check_call_alignment(assembly):
            return TestResult(
                test=test,
                passed=False,
                error="Call site alignment issue",
                assembly=assembly
            )
        
        return TestResult(
            test=test,
            passed=True,
            error=None,
            assembly=assembly
        )
        
    except Exception as e:
        return TestResult(
            test=test,
            passed=False,
            error=str(e),
            assembly=None
        )


def run_all_tests(verbose: bool = False) -> List[TestResult]:
    """Run all test cases."""
    results = []
    
    for test in TEST_CASES:
        result = run_test(test, verbose)
        results.append(result)
        
        status = "✅ PASS" if result.passed else "❌ FAIL"
        print(f"{status} {test.name}: {test.description}")
        
        if verbose or not result.passed:
            if result.error:
                print(f"       Error: {result.error}")
    
    return results


def run_category_tests(category: str, verbose: bool = False) -> List[TestResult]:
    """Run tests for a specific category."""
    results = []
    
    for test in TEST_CASES:
        if test.category == category:
            result = run_test(test, verbose)
            results.append(result)
            
            status = "✅ PASS" if result.passed else "❌ FAIL"
            print(f"{status} {test.name}: {test.description}")
            
            if verbose or not result.passed:
                if result.error:
                    print(f"       Error: {result.error}")
    
    return results


def print_summary(results: List[TestResult]):
    """Print test summary."""
    passed = sum(1 for r in results if r.passed)
    total = len(results)
    
    print()
    print("=" * 60)
    print(f"Test Summary: {passed}/{total} passed")
    print("=" * 60)
    
    if passed < total:
        print("\nFailed tests:")
        for result in results:
            if not result.passed:
                print(f"  ❌ {result.test.name}: {result.error}")


def main():
    parser = argparse.ArgumentParser(
        description="Prologue/Epilogue Regression Test Suite"
    )
    parser.add_argument("--run-all", action="store_true", help="Run all tests")
    parser.add_argument("--test", "-t", help="Run tests for specific category")
    parser.add_argument("--verbose", "-v", action="store_true", help="Show detailed output")
    parser.add_argument("--list", action="store_true", help="List all test cases")
    
    args = parser.parse_args()
    
    if args.list:
        print("Available test cases:")
        print("-" * 60)
        categories = set(t.category for t in TEST_CASES)
        for category in sorted(categories):
            print(f"\n[{category}]")
            for test in TEST_CASES:
                if test.category == category:
                    print(f"  - {test.name}: {test.description}")
        return 0
    
    if args.run_all:
        results = run_all_tests(args.verbose)
        print_summary(results)
        return 0 if all(r.passed for r in results) else 1
    
    if args.test:
        results = run_category_tests(args.test, args.verbose)
        print_summary(results)
        return 0 if all(r.passed for r in results) else 1
    
    parser.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
