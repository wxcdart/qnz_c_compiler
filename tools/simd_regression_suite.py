#!/usr/bin/env python3
"""
SIMD Regression Suite for ZeroCC
Runs SIMD-specific regression tests

Usage:
    python3 simd_regression_suite.py --run-all
    python3 simd_regression_suite.py --run <test_name>
    python3 simd_regression_suite.py --update-baseline
    python3 simd_regression_suite.py --report
"""

import sys
import os
import re
import json
import subprocess
import argparse
import tempfile
from typing import Dict, List, Optional, Tuple
from dataclasses import dataclass, asdict
from pathlib import Path


@dataclass
class RegressionTest:
    """A regression test case"""
    name: str
    description: str
    source_code: str
    expected_output: str
    expected_simd_count: int
    optimization_level: str = "-O3"


@dataclass
class TestResult:
    """Result of a regression test"""
    name: str
    passed: bool
    compiled: bool
    output_match: bool
    simd_count: int
    expected_simd_count: int
    execution_time: float
    error_message: str = ""


# Regression test cases
REGRESSION_TESTS = [
    RegressionTest(
        name="float_add_basic",
        description="Basic float addition should use addss",
        source_code="""
float float_add_basic(float a, float b) {
    return a + b;
}
int main(void) {
    float result = float_add_basic(1.0f, 2.0f);
    return (int)(result * 100);
}
""",
        expected_output="300",
        expected_simd_count=1,
    ),
    RegressionTest(
        name="float_sub_basic",
        description="Basic float subtraction should use subss",
        source_code="""
float float_sub_basic(float a, float b) {
    return a - b;
}
int main(void) {
    float result = float_sub_basic(3.0f, 1.0f);
    return (int)(result * 100);
}
""",
        expected_output="200",
        expected_simd_count=1,
    ),
    RegressionTest(
        name="float_mul_basic",
        description="Basic float multiplication should use mulss",
        source_code="""
float float_mul_basic(float a, float b) {
    return a * b;
}
int main(void) {
    float result = float_mul_basic(2.0f, 3.0f);
    return (int)(result * 100);
}
""",
        expected_output="600",
        expected_simd_count=1,
    ),
    RegressionTest(
        name="float_div_basic",
        description="Basic float division should use divss",
        source_code="""
float float_div_basic(float a, float b) {
    return a / b;
}
int main(void) {
    float result = float_div_basic(6.0f, 2.0f);
    return (int)(result * 100);
}
""",
        expected_output="300",
        expected_simd_count=1,
    ),
    RegressionTest(
        name="float_compare",
        description="Float comparison should use ucomiss",
        source_code="""
int float_compare(float a, float b) {
    return a < b;
}
int main(void) {
    return float_compare(1.0f, 2.0f);
}
""",
        expected_output="1",
        expected_simd_count=1,
    ),
    RegressionTest(
        name="float_array_loop",
        description="Float array loop should generate SIMD",
        source_code="""
int float_array_loop(int n) {
    float a[n], b[n];
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        sum += a[i] * b[i];
    }
    return (int)sum;
}
int main(void) { return float_array_loop(100); }
""",
        expected_output="0",
        expected_simd_count=2,
    ),
    RegressionTest(
        name="int_to_float_conv",
        description="Integer to float conversion should use cvtsi2ss",
        source_code="""
float int_to_float_conv(int a) {
    return (float)a;
}
int main(void) {
    float result = int_to_float_conv(42);
    return (int)(result * 100);
}
""",
        expected_output="4200",
        expected_simd_count=1,
    ),
    RegressionTest(
        name="float_to_int_conv",
        description="Float to integer conversion should use cvtss2si",
        source_code="""
int float_to_int_conv(float a) {
    return (int)a;
}
int main(void) {
    return float_to_int_conv(42.9f);
}
""",
        expected_output="42",
        expected_simd_count=1,
    ),
    RegressionTest(
        name="multiple_float_params",
        description="Multiple float parameters should use XMM registers",
        source_code="""
float multiple_float_params(float a, float b, float c, float d) {
    return a + b + c + d;
}
int main(void) {
    float result = multiple_float_params(1.0f, 2.0f, 3.0f, 4.0f);
    return (int)(result * 100);
}
""",
        expected_output="1000",
        expected_simd_count=3,
    ),
    RegressionTest(
        name="nested_float_ops",
        description="Nested float operations",
        source_code="""
float nested_float_ops(float a, float b) {
    return (a + b) * (a - b);
}
int main(void) {
    float result = nested_float_ops(3.0f, 1.0f);
    return (int)(result * 100);
}
""",
        expected_output="800",
        expected_simd_count=2,
    ),
]


class SIMDRegressionSuite:
    def __init__(self, zerocc_path: str = "./build/bin/zerocc", baseline_file: str = "simd_regression_baseline.json"):
        self.zerocc_path = zerocc_path
        self.baseline_file = baseline_file
        self.results: List[TestResult] = []
    
    def run_test(self, test: RegressionTest) -> TestResult:
        """Run a single regression test"""
        try:
            # Create temp files
            with tempfile.NamedTemporaryFile(mode='w', suffix='.c', delete=False) as f:
                f.write(test.source_code)
                src_file = f.name
            
            exe_file = src_file.replace('.c', '')
            asm_file = src_file.replace('.c', '.s')
            
            # Compile
            compile_result = subprocess.run(
                [self.zerocc_path, test.optimization_level, src_file, '-o', exe_file],
                capture_output=True,
                text=True,
                timeout=30
            )
            
            if compile_result.returncode != 0:
                return TestResult(
                    name=test.name,
                    passed=False,
                    compiled=False,
                    output_match=False,
                    simd_count=0,
                    expected_simd_count=test.expected_simd_count,
                    execution_time=0,
                    error_message=compile_result.stderr
                )
            
            # Count SIMD instructions
            simd_count = 0
            if os.path.exists(asm_file):
                with open(asm_file, 'r') as f:
                    asm_content = f.read()
                simd_patterns = [
                    r'\b(movaps|movups|movss|movsd|addss|addsd|subss|subsd|mulss|mulsd|divss|divsd|ucomiss|comiss|cvtsi2ss|cvtss2si)\b'
                ]
                for pattern in simd_patterns:
                    simd_count += len(re.findall(pattern, asm_content))
            
            # Run and check output
            import time
            start = time.perf_counter()
            run_result = subprocess.run(
                [exe_file],
                capture_output=True,
                text=True,
                timeout=10
            )
            elapsed = time.perf_counter() - start
            
            output_match = run_result.returncode == int(test.expected_output)
            simd_match = simd_count >= test.expected_simd_count
            
            passed = output_match and simd_match
            
            return TestResult(
                name=test.name,
                passed=passed,
                compiled=True,
                output_match=output_match,
                simd_count=simd_count,
                expected_simd_count=test.expected_simd_count,
                execution_time=elapsed,
                error_message="" if passed else f"Output: {run_result.returncode}, SIMD: {simd_count}"
            )
        
        except Exception as e:
            return TestResult(
                name=test.name,
                passed=False,
                compiled=False,
                output_match=False,
                simd_count=0,
                expected_simd_count=test.expected_simd_count,
                execution_time=0,
                error_message=str(e)
            )
        
        finally:
            # Cleanup
            for f in [src_file, exe_file, asm_file]:
                if os.path.exists(f):
                    os.remove(f)
    
    def run_all_tests(self) -> List[TestResult]:
        """Run all regression tests"""
        self.results = []
        
        for test in REGRESSION_TESTS:
            result = self.run_test(test)
            self.results.append(result)
        
        return self.results
    
    def load_baseline(self) -> Dict:
        """Load baseline results"""
        if os.path.exists(self.baseline_file):
            with open(self.baseline_file, 'r') as f:
                return json.load(f)
        return {}
    
    def save_baseline(self):
        """Save current results as baseline"""
        data = {r.name: asdict(r) for r in self.results}
        with open(self.baseline_file, 'w') as f:
            json.dump(data, f, indent=2)
    
    def check_regression(self) -> List[str]:
        """Check for regressions against baseline"""
        baseline = self.load_baseline()
        regressions = []
        
        for result in self.results:
            if result.name in baseline:
                baseline_result = baseline[result.name]
                
                # Check if SIMD count decreased
                if result.simd_count < baseline_result.get('simd_count', 0):
                    regressions.append(
                        f"{result.name}: SIMD count decreased from {baseline_result['simd_count']} to {result.simd_count}"
                    )
                
                # Check if test that was passing now fails
                if baseline_result.get('passed', False) and not result.passed:
                    regressions.append(
                        f"{result.name}: Test now failing (was passing)"
                    )
        
        return regressions
    
    def generate_report(self) -> str:
        """Generate regression report"""
        lines = []
        lines.append("=" * 70)
        lines.append("SIMD Regression Test Report")
        lines.append("=" * 70)
        
        if self.results:
            passed = sum(1 for r in self.results if r.passed)
            total = len(self.results)
            
            lines.append(f"\nTests Run: {total}")
            lines.append(f"Passed: {passed} ({passed/total*100:.1f}%)")
            lines.append(f"Failed: {total - passed}")
            
            # Check for regressions
            regressions = self.check_regression()
            if regressions:
                lines.append(f"\n⚠ Regressions Detected ({len(regressions)}):")
                lines.append("-" * 70)
                for reg in regressions:
                    lines.append(f"  {reg}")
            
            lines.append("\nDetailed Results:")
            lines.append("-" * 70)
            
            for result in self.results:
                status = "✓" if result.passed else "✗"
                simd_status = "✓" if result.simd_count >= result.expected_simd_count else "✗"
                
                lines.append(f"\n[{status}] {result.name}")
                lines.append(f"    Compiled: {result.compiled}")
                lines.append(f"    Output: {'✓' if result.output_match else '✗'}")
                lines.append(f"    SIMD: {result.simd_count}/{result.expected_simd_count} {simd_status}")
                if result.error_message:
                    lines.append(f"    Error: {result.error_message}")
        else:
            lines.append("\nNo tests run. Use --run-all to execute tests.")
        
        lines.append("=" * 70)
        return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(
        description='Run SIMD regression tests for ZeroCC'
    )
    parser.add_argument('--run-all', action='store_true',
                       help='Run all regression tests')
    parser.add_argument('--run', metavar='NAME',
                       help='Run specific test')
    parser.add_argument('--update-baseline', action='store_true',
                       help='Update baseline with current results')
    parser.add_argument('--report', action='store_true',
                       help='Generate report')
    parser.add_argument('--zerocc', metavar='PATH', default='./build/bin/zerocc',
                       help='Path to zerocc compiler')
    
    args = parser.parse_args()
    
    suite = SIMDRegressionSuite(args.zerocc)
    
    if args.run_all:
        print("Running SIMD regression tests...")
        suite.run_all_tests()
        
        if args.report or not args.update_baseline:
            print(suite.generate_report())
        
        if args.update_baseline:
            suite.save_baseline()
            print(f"Baseline saved to {suite.baseline_file}")
    
    elif args.run:
        for test in REGRESSION_TESTS:
            if test.name == args.run:
                result = suite.run_test(test)
                print(f"Test: {result.name}")
                print(f"  Passed: {result.passed}")
                print(f"  SIMD: {result.simd_count}/{result.expected_simd_count}")
                if result.error_message:
                    print(f"  Error: {result.error_message}")
                break
        else:
            print(f"Unknown test: {args.run}")
            print("Available tests:")
            for test in REGRESSION_TESTS:
                print(f"  - {test.name}")
    
    elif args.report:
        print(suite.generate_report())
    
    else:
        parser.print_help()
        sys.exit(1)


if __name__ == '__main__':
    main()
