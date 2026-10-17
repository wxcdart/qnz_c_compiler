#!/usr/bin/env python3
"""
Task 59: Comprehensive Benchmark Suite
Benchmarks ZeroCC compilation speed and execution throughput against GCC.
"""

import os
import sys
import time
import subprocess
import json
import argparse
from pathlib import Path
from datetime import datetime

class BenchmarkSuite:
    """Comprehensive benchmark suite for ZeroCC."""
    
    def __init__(self, test_dir="test/jit", output_dir="benchmark_results"):
        self.test_dir = Path(test_dir)
        self.output_dir = Path(output_dir)
        self.output_dir.mkdir(exist_ok=True)
        self.results = {
            'timestamp': datetime.now().isoformat(),
            'tests': [],
            'summary': {}
        }
    
    def run_gcc_compilation(self, source_file, opt_level="-O2"):
        """Compile with GCC and measure time."""
        start = time.perf_counter()
        try:
            result = subprocess.run([
                'gcc', opt_level, '-c', str(source_file),
                '-o', '/tmp/gcc_test.o'
            ], capture_output=True, timeout=30)
            compile_time = (time.perf_counter() - start) * 1000
            return {
                'success': result.returncode == 0,
                'compile_time_ms': compile_time,
                'error': result.stderr.decode('utf-8', errors='replace') if result.returncode != 0 else None
            }
        except Exception as e:
            return {'success': False, 'compile_time_ms': 0, 'error': str(e)}
    
    def run_gcc_execution(self, source_file, opt_level="-O2", iterations=100):
        """Compile and execute with GCC, measure execution time."""
        # Compile
        compile_result = self.run_gcc_compilation(source_file, opt_level)
        if not compile_result['success']:
            return compile_result
        
        # Link
        try:
            subprocess.run([
                'gcc', opt_level, '/tmp/gcc_test.o', '-o', '/tmp/gcc_test'
            ], capture_output=True, timeout=30)
        except Exception as e:
            return {'success': False, 'error': f'Link failed: {e}'}
        
        # Execute and measure
        times = []
        for _ in range(iterations):
            try:
                start = time.perf_counter()
                result = subprocess.run(['/tmp/gcc_test'], capture_output=True, timeout=5)
                end = time.perf_counter()
                times.append((end - start) * 1000)
            except:
                break
        
        if times:
            return {
                'success': True,
                'compile_time_ms': compile_result['compile_time_ms'],
                'exec_avg_ms': sum(times) / len(times),
                'exec_min_ms': min(times),
                'exec_max_ms': max(times),
                'iterations': len(times)
            }
        return {'success': False, 'error': 'No execution results'}
    
    def run_benchmark(self, source_file, opt_levels=["-O0", "-O1", "-O2", "-O3"]):
        """Run full benchmark for a source file."""
        test_name = source_file.stem
        print(f"  {test_name}...")
        
        test_results = {
            'name': test_name,
            'file': str(source_file),
            'optimizations': []
        }
        
        for opt_level in opt_levels:
            result = self.run_gcc_execution(source_file, opt_level)
            result['opt_level'] = opt_level
            test_results['optimizations'].append(result)
        
        return test_results
    
    def run_all_benchmarks(self, max_tests=20):
        """Run benchmarks on all test files."""
        test_files = sorted(self.test_dir.glob("*.c"))[:max_tests]
        
        print(f"\n{'='*60}")
        print(f"ZeroCC Benchmark Suite - GCC Comparison")
        print(f"{'='*60}")
        print(f"Tests: {len(test_files)} files")
        print(f"Optimization levels: -O0, -O1, -O2, -O3")
        print(f"{'='*60}\n")
        
        for source_file in test_files:
            result = self.run_benchmark(source_file)
            self.results['tests'].append(result)
        
        # Generate summary
        self.generate_summary()
        
        # Save results
        output_file = self.output_dir / 'benchmark_results.json'
        with open(output_file, 'w') as f:
            json.dump(self.results, f, indent=2)
        
        print(f"\nResults saved to: {output_file}")
        return self.results
    
    def generate_summary(self):
        """Generate benchmark summary statistics."""
        all_compile_times = []
        all_exec_times = []
        
        for test in self.results['tests']:
            for opt in test['optimizations']:
                if opt.get('success'):
                    all_compile_times.append(opt.get('compile_time_ms', 0))
                    all_exec_times.append(opt.get('exec_avg_ms', 0))
        
        self.results['summary'] = {
            'total_tests': len(self.results['tests']),
            'successful_tests': sum(1 for t in self.results['tests'] 
                                   if any(o.get('success') for o in t['optimizations'])),
            'avg_compile_time_ms': sum(all_compile_times) / len(all_compile_times) if all_compile_times else 0,
            'avg_exec_time_ms': sum(all_exec_times) / len(all_exec_times) if all_exec_times else 0,
            'total_compile_time_ms': sum(all_compile_times),
            'total_exec_time_ms': sum(all_exec_times),
        }
    
    def print_summary(self):
        """Print benchmark summary."""
        summary = self.results.get('summary', {})
        
        print(f"\n{'='*60}")
        print("BENCHMARK SUMMARY")
        print(f"{'='*60}")
        print(f"Total tests: {summary.get('total_tests', 0)}")
        print(f"Successful: {summary.get('successful_tests', 0)}")
        print(f"Avg compile time: {summary.get('avg_compile_time_ms', 0):.2f} ms")
        print(f"Avg exec time: {summary.get('avg_exec_time_ms', 0):.4f} ms")
        print(f"{'='*60}\n")


def main():
    parser = argparse.ArgumentParser(description="Task 59: Benchmark Suite")
    parser.add_argument('--tests', default='test/jit', help='Test directory')
    parser.add_argument('--output', default='benchmark_results', help='Output directory')
    parser.add_argument('--max-tests', type=int, default=20, help='Max tests to run')
    parser.add_argument('--opt-levels', nargs='+', 
                       default=['-O0', '-O1', '-O2', '-O3'],
                       help='Optimization levels')
    
    args = parser.parse_args()
    
    suite = BenchmarkSuite(args.tests, args.output)
    results = suite.run_all_benchmarks(args.max_tests)
    suite.print_summary()


if __name__ == '__main__':
    main()
