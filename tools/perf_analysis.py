#!/usr/bin/env python3
"""
Task 59: Performance Analysis with perf and valgrind
Analyzes compilation and execution performance using profiling tools.
"""

import os
import sys
import time
import subprocess
import json
import argparse
from pathlib import Path
from datetime import datetime

class PerfAnalyzer:
    """Performance analysis using perf and valgrind."""
    
    def __init__(self, test_dir="test/jit", output_dir="perf_results"):
        self.test_dir = Path(test_dir)
        self.output_dir = Path(output_dir)
        self.output_dir.mkdir(exist_ok=True)
        self.results = {
            'timestamp': datetime.now().isoformat(),
            'tests': [],
            'summary': {}
        }
    
    def run_perf_compile(self, source_file, opt_level="-O2"):
        """Profile GCC compilation with perf."""
        try:
            start = time.perf_counter()
            result = subprocess.run([
                'perf', 'record', '-o', '/tmp/perf_compile.data',
                '-g', 'gcc', opt_level, '-c', str(source_file), '-o', '/tmp/perf_test.o'
            ], capture_output=True, timeout=60)
            compile_time = (time.perf_counter() - start) * 1000
            
            # Get perf report
            report = subprocess.run([
                'perf', 'report', '-i', '/tmp/perf_compile.data', '--stdio'
            ], capture_output=True, timeout=30)
            
            return {
                'success': result.returncode == 0,
                'compile_time_ms': compile_time,
                'perf_data': '/tmp/perf_compile.data',
                'top_functions': self.parse_perf_report(report.stdout.decode('utf-8', errors='replace'))
            }
        except Exception as e:
            return {'success': False, 'error': str(e)}
    
    def parse_perf_report(self, report_text):
        """Parse perf report to extract top functions."""
        lines = report_text.split('\n')
        top_functions = []
        
        for line in lines:
            if '%' in line and 'gcov' not in line.lower():
                parts = line.split()
                if len(parts) >= 2:
                    try:
                        pct = float(parts[0].replace('%', ''))
                        func = ' '.join(parts[1:]) if len(parts) > 1 else 'unknown'
                        top_functions.append({'percentage': pct, 'function': func})
                    except:
                        pass
        
        return top_functions[:10]  # Top 10 functions
    
    def run_valgrind(self, source_file, opt_level="-O2"):
        """Profile with valgrind (callgrind or cachegrind)."""
        try:
            # Compile first
            compile_result = subprocess.run([
                'gcc', opt_level, '-O0', '-g', str(source_file), '-o', '/tmp/valgrind_test'
            ], capture_output=True, timeout=30)
            
            if compile_result.returncode != 0:
                return {'success': False, 'error': 'compile'}
            
            # Run with valgrind callgrind
            start = time.perf_counter()
            result = subprocess.run([
                'valgrind', '--tool=callgrind', '--callgrind-out-file=/tmp/callgrind.out',
                '/tmp/valgrind_test'
            ], capture_output=True, timeout=60)
            valgrind_time = (time.perf_counter() - start) * 1000
            
            return {
                'success': True,
                'valgrind_time_ms': valgrind_time,
                'callgrind_data': '/tmp/callgrind.out'
            }
        except Exception as e:
            return {'success': False, 'error': str(e)}
    
    def analyze_test(self, source_file, opt_level="-O2"):
        """Analyze a single test with perf and valgrind."""
        test_name = source_file.stem
        print(f"  {test_name}...")
        
        results = {
            'name': test_name,
            'file': str(source_file),
            'opt_level': opt_level,
            'perf': self.run_perf_compile(source_file, opt_level),
            'valgrind': self.run_valgrind(source_file, opt_level)
        }
        
        return results
    
    def analyze_all(self, opt_level="-O2", max_tests=10):
        """Analyze all tests with perf and valgrind."""
        test_files = sorted(self.test_dir.glob("*.c"))[:max_tests]
        
        print(f"\n{'='*60}")
        print(f"Performance Analysis - perf + valgrind")
        print(f"{'='*60}")
        print(f"Tests: {len(test_files)} files")
        print(f"Optimization level: {opt_level}")
        print(f"{'='*60}\n")
        
        for source_file in test_files:
            result = self.analyze_test(source_file, opt_level)
            self.results['tests'].append(result)
        
        # Generate summary
        self.generate_summary()
        
        # Save results
        output_file = self.output_dir / f'perf_analysis_{opt_level.replace("-", "")}.json'
        with open(output_file, 'w') as f:
            json.dump(self.results, f, indent=2)
        
        print(f"\nResults saved to: {output_file}")
        return self.results
    
    def generate_summary(self):
        """Generate analysis summary."""
        perf_times = [t['perf'].get('compile_time_ms', 0) 
                     for t in self.results['tests'] 
                     if t['perf'].get('success')]
        
        self.results['summary'] = {
            'total_tests': len(self.results['tests']),
            'perf_success': len(perf_times),
            'avg_perf_time_ms': sum(perf_times) / len(perf_times) if perf_times else 0,
        }
    
    def print_summary(self):
        """Print analysis summary."""
        summary = self.results.get('summary', {})
        
        print(f"\n{'='*60}")
        print("PERFORMANCE ANALYSIS SUMMARY")
        print(f"{'='*60}")
        print(f"Total tests: {summary.get('total_tests', 0)}")
        print(f"perf success: {summary.get('perf_success', 0)}")
        print(f"Avg perf time: {summary.get('avg_perf_time_ms', 0):.2f} ms")
        print(f"{'='*60}\n")


def main():
    parser = argparse.ArgumentParser(description="Task 59: Performance Analysis")
    parser.add_argument('--tests', default='test/jit', help='Test directory')
    parser.add_argument('--output', default='perf_results', help='Output directory')
    parser.add_argument('--opt-level', default='-O2', help='Optimization level')
    parser.add_argument('--max-tests', type=int, default=10, help='Max tests to analyze')
    
    args = parser.parse_args()
    
    analyzer = PerfAnalyzer(args.tests, args.output)
    results = analyzer.analyze_all(args.opt_level, args.max_tests)
    analyzer.print_summary()


if __name__ == '__main__':
    main()
