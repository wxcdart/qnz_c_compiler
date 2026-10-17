#!/usr/bin/env python3
"""
SIMD Benchmark Runner for ZeroCC
Measures SIMD performance improvements

Usage:
    python3 simd_benchmark_runner.py --run-all
    python3 simd_benchmark_runner.py --benchmark <test_file>
    python3 simd_benchmark_runner.py --compare <optimized> <unoptimized>
"""

import sys
import os
import re
import json
import time
import subprocess
import argparse
import tempfile
from typing import Dict, List, Optional, Tuple
from dataclasses import dataclass, asdict


@dataclass
class BenchmarkResult:
    """Result of a benchmark run"""
    name: str
    description: str
    optimized_time: float
    unoptimized_time: float
    speedup: float
    simd_instructions: int
    passed: bool
    notes: str = ""


# Benchmark test cases
BENCHMARK_TESTS = [
    {
        'name': 'array_sum',
        'description': 'Sum array elements',
        'code': '''
int array_sum(int n) {{
    int arr[n];
    int sum = 0;
    for (int i = 0; i < n; i++) {{
        arr[i] = i;
        sum += arr[i];
    }}
    return sum;
}}
int main(void) {{ return array_sum(10000); }}
'''
    },
    {
        'name': 'array_multiply',
        'description': 'Element-wise array multiplication',
        'code': '''
int array_multiply(int n) {{
    int a[n], b[n], c[n];
    int sum = 0;
    for (int i = 0; i < n; i++) {{
        c[i] = a[i] * b[i];
        sum += c[i];
    }}
    return sum;
}}
int main(void) {{ return array_multiply(10000); }}
'''
    },
    {
        'name': 'float_dot_product',
        'description': 'Float dot product with reduction',
        'code': '''
int float_dot_product(int n) {{
    float a[n], b[n];
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {{
        sum += a[i] * b[i];
    }}
    return (int)sum;
}}
int main(void) {{ return float_dot_product(10000); }}
'''
    },
    {
        'name': 'array_transform',
        'description': 'Array element transformation',
        'code': '''
int array_transform(int n) {{
    int a[n], b[n];
    for (int i = 0; i < n; i++) {{
        b[i] = a[i] * 2 + 1;
    }}
    return b[0];
}}
int main(void) {{ return array_transform(10000); }}
'''
    },
    {
        'name': 'conditional_array',
        'description': 'Array with conditional (may not vectorize)',
        'code': '''
int conditional_array(int n) {{
    int a[n], b[n];
    for (int i = 0; i < n; i++) {{
        if (a[i] > 0) {{
            b[i] = a[i] * 2;
        }} else {{
            b[i] = a[i];
        }}
    }}
    return b[0];
}}
int main(void) {{ return conditional_array(10000); }}
'''
    },
]


class SIMDBenchmarkRunner:
    def __init__(self, zerocc_path: str = "./build/bin/zerocc"):
        self.zerocc_path = zerocc_path
        self.results: List[BenchmarkResult] = []
    
    def compile_and_run(self, source_code: str, opt_level: str) -> Tuple[bool, float, int]:
        """Compile and run a benchmark, return success, time, and SIMD count"""
        try:
            # Create temp files
            with tempfile.NamedTemporaryFile(mode='w', suffix='.c', delete=False) as f:
                f.write(source_code)
                src_file = f.name
            
            exe_file = src_file.replace('.c', '')
            asm_file = src_file.replace('.c', '.s')
            
            # Compile
            compile_result = subprocess.run(
                [self.zerocc_path, opt_level, src_file, '-o', exe_file],
                capture_output=True,
                text=True,
                timeout=30
            )
            
            if compile_result.returncode != 0:
                os.remove(src_file)
                return False, 0.0, 0
            
            # Count SIMD instructions in assembly
            simd_count = 0
            if os.path.exists(asm_file):
                with open(asm_file, 'r') as f:
                    asm_content = f.read()
                simd_patterns = [
                    r'\b(movaps|movups|movss|movsd|addss|addsd|subss|subsd|mulss|mulsd|divss|divsd)\b'
                ]
                for pattern in simd_patterns:
                    simd_count += len(re.findall(pattern, asm_content))
            
            # Run and time
            start = time.perf_counter()
            run_result = subprocess.run(
                [exe_file],
                capture_output=True,
                timeout=10
            )
            elapsed = time.perf_counter() - start
            
            passed = run_result.returncode == 0
            
            # Cleanup
            for f in [src_file, exe_file, asm_file]:
                if os.path.exists(f):
                    os.remove(f)
            
            return passed, elapsed, simd_count
        
        except Exception as e:
            return False, 0.0, 0
    
    def run_benchmark(self, test: Dict) -> BenchmarkResult:
        """Run a single benchmark"""
        name = test['name']
        description = test['description']
        code = test['code']
        
        # Run optimized
        opt_passed, opt_time, simd_count = self.compile_and_run(code, '-O3')
        
        # Run unoptimized
        unopt_passed, unopt_time, _ = self.compile_and_run(code, '-O0')
        
        # Calculate speedup
        speedup = unopt_time / opt_time if opt_time > 0 else 1.0
        
        # Determine if benchmark passed
        passed = opt_passed and unopt_passed
        
        return BenchmarkResult(
            name=name,
            description=description,
            optimized_time=opt_time,
            unoptimized_time=unopt_time,
            speedup=speedup,
            simd_instructions=simd_count,
            passed=passed,
            notes=f"Optimized: {opt_time*1000:.2f}ms, Unoptimized: {unopt_time*1000:.2f}ms"
        )
    
    def run_all_benchmarks(self) -> List[BenchmarkResult]:
        """Run all benchmarks"""
        self.results = []
        
        for test in BENCHMARK_TESTS:
            result = self.run_benchmark(test)
            self.results.append(result)
            print(f"  {result.name}: {result.speedup:.2f}x speedup ({result.simd_instructions} SIMD instr)")
        
        return self.results
    
    def compare_implementations(self, opt_file: str, unopt_file: str) -> Dict:
        """Compare optimized vs unoptimized assembly"""
        comparison = {
            'optimized': {'simd_count': 0, 'instructions': 0},
            'unoptimized': {'simd_count': 0, 'instructions': 0},
            'simd_ratio': 0.0
        }
        
        for label, filepath in [('optimized', opt_file), ('unoptimized', unopt_file)]:
            try:
                with open(filepath, 'r') as f:
                    content = f.read()
                
                # Count total instructions
                lines = [l.strip() for l in content.split('\n') if l.strip() and not l.strip().startswith('#')]
                comparison[label]['instructions'] = len(lines)
                
                # Count SIMD instructions
                simd_patterns = [
                    r'\b(movaps|movups|movss|movsd|addss|addsd|subss|subsd|mulss|mulsd|divss|divsd)\b'
                ]
                simd_count = 0
                for pattern in simd_patterns:
                    simd_count += len(re.findall(pattern, content))
                comparison[label]['simd_count'] = simd_count
                
            except:
                pass
        
        if comparison['unoptimized']['instructions'] > 0:
            comparison['simd_ratio'] = comparison['optimized']['simd_count'] / comparison['unoptimized']['instructions']
        
        return comparison
    
    def generate_report(self) -> str:
        """Generate benchmark report"""
        lines = []
        lines.append("=" * 70)
        lines.append("SIMD Benchmark Report")
        lines.append("=" * 70)
        
        if self.results:
            avg_speedup = sum(r.speedup for r in self.results) / len(self.results)
            total_simd = sum(r.simd_instructions for r in self.results)
            passed = sum(1 for r in self.results if r.passed)
            
            lines.append(f"\nBenchmarks Run: {len(self.results)}")
            lines.append(f"Passed: {passed}/{len(self.results)}")
            lines.append(f"Average Speedup: {avg_speedup:.2f}x")
            lines.append(f"Total SIMD Instructions: {total_simd}")
            
            lines.append("\nDetailed Results:")
            lines.append("-" * 70)
            
            for result in self.results:
                status = "✓" if result.passed else "✗"
                lines.append(f"\n[{status}] {result.name}")
                lines.append(f"    Description: {result.description}")
                lines.append(f"    Optimized:   {result.optimized_time*1000:.2f}ms")
                lines.append(f"    Unoptimized: {result.unoptimized_time*1000:.2f}ms")
                lines.append(f"    Speedup:     {result.speedup:.2f}x")
                lines.append(f"    SIMD Instr:  {result.simd_instructions}")
                if result.notes:
                    lines.append(f"    Notes:       {result.notes}")
        else:
            lines.append("\nNo benchmarks run. Use --run-all to execute benchmarks.")
        
        lines.append("=" * 70)
        return '\n'.join(lines)
    
    def generate_json_report(self) -> str:
        """Generate JSON report"""
        if self.results:
            return json.dumps([asdict(r) for r in self.results], indent=2)
        return json.dumps([])


def main():
    parser = argparse.ArgumentParser(
        description='Run SIMD benchmarks for ZeroCC'
    )
    parser.add_argument('--run-all', action='store_true',
                       help='Run all benchmarks')
    parser.add_argument('--benchmark', metavar='FILE',
                       help='Run specific benchmark file')
    parser.add_argument('--compare', nargs=2, metavar=('OPT', 'UNOPT'),
                       help='Compare optimized vs unoptimized assembly')
    parser.add_argument('--json', action='store_true',
                       help='Output JSON report')
    parser.add_argument('--zerocc', metavar='PATH', default='./build/bin/zerocc',
                       help='Path to zerocc compiler')
    
    args = parser.parse_args()
    
    runner = SIMDBenchmarkRunner(args.zerocc)
    
    if args.run_all:
        print("Running SIMD benchmarks...")
        runner.run_all_benchmarks()
        
        if args.json:
            print(runner.generate_json_report())
        else:
            print(runner.generate_report())
    
    elif args.benchmark:
        # Run single benchmark file
        try:
            with open(args.benchmark, 'r') as f:
                code = f.read()
            
            opt_passed, opt_time, simd = runner.compile_and_run(code, '-O3')
            unopt_passed, unopt_time, _ = runner.compile_and_run(code, '-O0')
            
            speedup = unopt_time / opt_time if opt_time > 0 else 1.0
            
            print(f"Benchmark: {args.benchmark}")
            print(f"  Optimized:   {opt_time*1000:.2f}ms")
            print(f"  Unoptimized: {unopt_time*1000:.2f}ms")
            print(f"  Speedup:     {speedup:.2f}x")
            print(f"  SIMD Instr:  {simd}")
        
        except Exception as e:
            print(f"Error: {e}")
            sys.exit(1)
    
    elif args.compare:
        comparison = runner.compare_implementations(args.compare[0], args.compare[1])
        print("Assembly Comparison:")
        print(f"  Optimized SIMD:    {comparison['optimized']['simd_count']}")
        print(f"  Unoptimized SIMD:  {comparison['unoptimized']['simd_count']}")
        print(f"  SIMD Ratio:        {comparison['simd_ratio']:.2%}")
    
    else:
        parser.print_help()
        sys.exit(1)


if __name__ == '__main__':
    main()
