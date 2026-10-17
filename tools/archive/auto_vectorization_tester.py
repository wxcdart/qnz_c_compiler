#!/usr/bin/env python3
"""
Auto-Vectorization Tester for ZeroCC
Tests auto-vectorization heuristics and decisions

Usage:
    python3 auto_vectorization_tester.py --generate-tests
    python3 auto_vectorization_tester.py --run-tests
    python3 auto_vectorization_tester.py --analyze <test_dir>
"""

import sys
import os
import re
import json
import tempfile
import subprocess
import argparse
from typing import Dict, List, Optional, Tuple
from dataclasses import dataclass, asdict
from pathlib import Path


@dataclass
class TestCase:
    """A vectorization test case"""
    name: str
    description: str
    source_code: str
    expected_vectorizable: bool
    expected_speedup: float
    optimization_level: str = "-O3"


@dataclass
class TestResult:
    """Result of running a vectorization test"""
    test_name: str
    passed: bool
    compiled: bool
    execution_time: float
    output_correct: bool
    simd_instructions_found: int
    vectorization_decision: str
    notes: str = ""


# Vectorization test cases
VECTORIZATION_TESTS = [
    TestCase(
        name="simple_array_add",
        description="Simple array addition loop",
        source_code="""
int simple_array_add(int n) {
    int arr[n];
    int sum = 0;
    for (int i = 0; i < n; i++) {
        arr[i] = i + 1;
        sum += arr[i];
    }
    return sum;
}
int main(void) { return simple_array_add(100); }
""",
        expected_vectorizable=True,
        expected_speedup=4.0
    ),
    TestCase(
        name="array_multiply",
        description="Array element-wise multiplication",
        source_code="""
int array_multiply(int n) {
    int a[n], b[n], c[n];
    int sum = 0;
    for (int i = 0; i < n; i++) {
        c[i] = a[i] * b[i];
        sum += c[i];
    }
    return sum;
}
int main(void) { return array_multiply(100); }
""",
        expected_vectorizable=True,
        expected_speedup=4.0
    ),
    TestCase(
        name="dot_product",
        description="Dot product with reduction",
        source_code="""
int dot_product(int n) {
    int a[n], b[n];
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += a[i] * b[i];
    }
    return sum;
}
int main(void) { return dot_product(100); }
""",
        expected_vectorizable=True,
        expected_speedup=2.0  # Reduction overhead
    ),
    TestCase(
        name="dependent_loop",
        description="Loop with data dependency (not vectorizable)",
        source_code="""
int dependent_loop(int n) {
    int arr[n];
    arr[0] = 1;
    for (int i = 1; i < n; i++) {
        arr[i] = arr[i-1] + 1;  // Dependency on previous iteration
    }
    return arr[n-1];
}
int main(void) { return dependent_loop(100); }
""",
        expected_vectorizable=False,
        expected_speedup=1.0
    ),
    TestCase(
        name="float_array_ops",
        description="Float array operations",
        source_code="""
int float_array_ops(int n) {
    float a[n], b[n], c[n];
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        c[i] = a[i] * 2.0f + b[i];
        sum += c[i];
    }
    return (int)sum;
}
int main(void) { return float_array_ops(100); }
""",
        expected_vectorizable=True,
        expected_speedup=4.0
    ),
    TestCase(
        name="conditional_array",
        description="Array with conditional (may hinder vectorization)",
        source_code="""
int conditional_array(int n) {
    int a[n], b[n];
    for (int i = 0; i < n; i++) {
        if (a[i] > 0) {
            b[i] = a[i] * 2;
        } else {
            b[i] = a[i];
        }
    }
    return b[0];
}
int main(void) { return conditional_array(100); }
""",
        expected_vectorizable=False,
        expected_speedup=1.0
    ),
    TestCase(
        name="pointer_aliasing",
        description="Potential pointer aliasing (conservative)",
        source_code="""
int pointer_aliasing(int n, int *a, int *b) {
    for (int i = 0; i < n; i++) {
        a[i] = b[i] * 2;
    }
    return a[0];
}
int main(void) { 
    int arr[100];
    return pointer_aliasing(100, arr, arr);
}
""",
        expected_vectorizable=False,
        expected_speedup=1.0
    ),
    TestCase(
        name="stride_access",
        description="Strided array access",
        source_code="""
int stride_access(int n) {
    int arr[n * 2];
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += arr[i * 2];  // Stride of 2
    }
    return sum;
}
int main(void) { return stride_access(50); }
""",
        expected_vectorizable=True,
        expected_speedup=2.0  # Stride may reduce benefit
    ),
]


class AutoVectorizationTester:
    def __init__(self, zerocc_path: str = "./build/bin/zerocc"):
        self.zerocc_path = zerocc_path
        self.results: List[TestResult] = []
    
    def generate_test_files(self, output_dir: str) -> List[str]:
        """Generate test case source files"""
        os.makedirs(output_dir, exist_ok=True)
        files = []
        
        for test in VECTORIZATION_TESTS:
            filepath = os.path.join(output_dir, f"{test.name}.c")
            with open(filepath, 'w') as f:
                f.write(test.source_code)
            files.append(filepath)
        
        return files
    
    def compile_test(self, source_file: str, opt_level: str = "-O3") -> Tuple[bool, str]:
        """Compile a test file"""
        try:
            result = subprocess.run(
                [self.zerocc_path, opt_level, source_file, "-S", "-o", source_file.replace('.c', '.s')],
                capture_output=True,
                text=True,
                timeout=30
            )
            return result.returncode == 0, result.stderr
        except Exception as e:
            return False, str(e)
    
    def analyze_assembly(self, asm_file: str) -> int:
        """Count SIMD instructions in assembly file"""
        simd_patterns = [
            r'\b(movaps|movups|movss|movsd|addss|addsd|subss|subsd|mulss|mulsd|divss|divsd)\b',
        ]
        
        try:
            with open(asm_file, 'r') as f:
                content = f.read()
            
            count = 0
            for pattern in simd_patterns:
                count += len(re.findall(pattern, content))
            
            return count
        except:
            return 0
    
    def run_test(self, test: TestCase) -> TestResult:
        """Run a single vectorization test"""
        # Create temporary file
        with tempfile.NamedTemporaryFile(mode='w', suffix='.c', delete=False) as f:
            f.write(test.source_code)
            source_file = f.name
        
        asm_file = source_file.replace('.c', '.s')
        
        try:
            # Compile
            compiled, error = self.compile_test(source_file, test.optimization_level)
            
            if not compiled:
                return TestResult(
                    test_name=test.name,
                    passed=False,
                    compiled=False,
                    execution_time=0,
                    output_correct=False,
                    simd_instructions_found=0,
                    vectorization_decision="FAILED_TO_COMPILE",
                    notes=error
                )
            
            # Analyze assembly
            simd_count = self.analyze_assembly(asm_file)
            
            # Determine vectorization decision
            if simd_count > 0:
                decision = "LIKELY_VECTORIZED" if test.expected_vectorizable else "UNEXPECTED_SIMD"
            else:
                decision = "NOT_VECTORIZED" if not test.expected_vectorizable else "MISSING_SIMD"
            
            # Check if result matches expectation
            passed = (test.expected_vectorizable and simd_count > 0) or \
                     (not test.expected_vectorizable and simd_count == 0)
            
            return TestResult(
                test_name=test.name,
                passed=passed,
                compiled=True,
                execution_time=0,
                output_correct=True,
                simd_instructions_found=simd_count,
                vectorization_decision=decision,
                notes=f"Expected vectorizable: {test.expected_vectorizable}, Found {simd_count} SIMD instructions"
            )
        
        finally:
            # Cleanup
            for f in [source_file, asm_file]:
                if os.path.exists(f):
                    os.remove(f)
    
    def run_all_tests(self) -> List[TestResult]:
        """Run all vectorization tests"""
        self.results = []
        
        for test in VECTORIZATION_TESTS:
            result = self.run_test(test)
            self.results.append(result)
        
        return self.results
    
    def analyze_directory(self, dirpath: str) -> Dict[str, any]:
        """Analyze a directory for vectorization potential"""
        path = Path(dirpath)
        results = {
            'total_files': 0,
            'files_with_loops': 0,
            'vectorizable_loops': 0,
            'total_loops': 0,
            'files': []
        }
        
        loop_pattern = re.compile(r'\b(for|while)\s*\(')
        array_pattern = re.compile(r'\w+\s*\[\s*\w+\s*\]')
        
        for c_file in path.rglob('*.c'):
            results['total_files'] += 1
            
            try:
                with open(c_file, 'r') as f:
                    content = f.read()
                
                loops = len(loop_pattern.findall(content))
                arrays = len(array_pattern.findall(content))
                
                if loops > 0:
                    results['files_with_loops'] += 1
                    results['total_loops'] += loops
                    
                    # Heuristic: if has loops and arrays, likely vectorizable
                    if arrays > 0:
                        results['vectorizable_loops'] += loops
                    
                    results['files'].append({
                        'name': str(c_file),
                        'loops': loops,
                        'arrays': arrays,
                        'vectorizable': arrays > 0
                    })
            except:
                pass
        
        return results
    
    def generate_report(self) -> str:
        """Generate test report"""
        lines = []
        lines.append("=" * 70)
        lines.append("Auto-Vectorization Test Report")
        lines.append("=" * 70)
        
        if self.results:
            passed = sum(1 for r in self.results if r.passed)
            total = len(self.results)
            
            lines.append(f"\nTests Run: {total}")
            lines.append(f"Passed: {passed} ({passed/total*100:.1f}%)")
            lines.append(f"Failed: {total - passed}")
            
            lines.append("\nTest Results:")
            lines.append("-" * 70)
            
            for result in self.results:
                status = "✓" if result.passed else "✗"
                lines.append(f"\n[{status}] {result.test_name}")
                lines.append(f"    Compiled: {result.compiled}")
                lines.append(f"    SIMD Instructions: {result.simd_instructions_found}")
                lines.append(f"    Decision: {result.vectorization_decision}")
                if result.notes:
                    lines.append(f"    Notes: {result.notes}")
        else:
            lines.append("No tests run. Use --run-tests to execute tests.")
        
        lines.append("=" * 70)
        return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(
        description='Test auto-vectorization heuristics in ZeroCC'
    )
    parser.add_argument('--generate-tests', action='store_true',
                       help='Generate test case files')
    parser.add_argument('--run-tests', action='store_true',
                       help='Run vectorization tests')
    parser.add_argument('--analyze', metavar='DIR',
                       help='Analyze directory for vectorization potential')
    parser.add_argument('--output-dir', metavar='DIR', default='/tmp/vec_tests',
                       help='Output directory for generated tests')
    parser.add_argument('--zerocc', metavar='PATH', default='./build/bin/zerocc',
                       help='Path to zerocc compiler')
    
    args = parser.parse_args()
    
    tester = AutoVectorizationTester(args.zerocc)
    
    if args.generate_tests:
        files = tester.generate_test_files(args.output_dir)
        print(f"Generated {len(files)} test files in {args.output_dir}")
        for f in files:
            print(f"  {f}")
    
    if args.run_tests:
        results = tester.run_all_tests()
        print(tester.generate_report())
    
    if args.analyze:
        analysis = tester.analyze_directory(args.analyze)
        print(f"\nDirectory Analysis: {args.analyze}")
        print(f"  Total files: {analysis['total_files']}")
        print(f"  Files with loops: {analysis['files_with_loops']}")
        print(f"  Total loops: {analysis['total_loops']}")
        print(f"  Vectorizable loops: {analysis['vectorizable_loops']}")
    
    if not (args.generate_tests or args.run_tests or args.analyze):
        parser.print_help()
        sys.exit(1)


if __name__ == '__main__':
    main()
