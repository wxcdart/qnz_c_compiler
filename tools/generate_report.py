#!/usr/bin/env python3
"""
Task 59: Benchmark Report Generator
Generates comprehensive HTML/PDF reports from benchmark results.
"""

import os
import sys
import json
import argparse
from pathlib import Path
from datetime import datetime

class ReportGenerator:
    """Generate benchmark reports."""
    
    def __init__(self, results_dir="benchmark_results", output_dir="reports"):
        self.results_dir = Path(results_dir)
        self.output_dir = Path(output_dir)
        self.output_dir.mkdir(exist_ok=True)
    
    def load_results(self, filename):
        """Load benchmark results from JSON file."""
        filepath = self.results_dir / filename
        if not filepath.exists():
            return None
        with open(filepath, 'r') as f:
            return json.load(f)
    
    def generate_html_report(self, benchmark_data, diff_data=None, perf_data=None):
        """Generate HTML report."""
        timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        
        html = f'''<!DOCTYPE html>
<html>
<head>
    <title>ZeroCC Benchmark Report</title>
    <style>
        body {{ font-family: Arial, sans-serif; margin: 40px; }}
        h1 {{ color: #333; }}
        h2 {{ color: #666; border-bottom: 1px solid #ddd; }}
        table {{ border-collapse: collapse; width: 100%; margin: 20px 0; }}
        th, td {{ border: 1px solid #ddd; padding: 8px; text-align: left; }}
        th {{ background-color: #4CAF50; color: white; }}
        tr:nth-child(even) {{ background-color: #f2f2f2; }}
        .pass {{ color: green; }}
        .fail {{ color: red; }}
        .summary {{ background-color: #f9f9f9; padding: 20px; border-radius: 5px; }}
    </style>
</head>
<body>
    <h1>ZeroCC Benchmark Report</h1>
    <p>Generated: {timestamp}</p>
'''
        
        # Benchmark summary
        if benchmark_data:
            summary = benchmark_data.get('summary', {})
            html += f'''
    <h2>Benchmark Summary</h2>
    <div class="summary">
        <p><strong>Total Tests:</strong> {summary.get('total_tests', 0)}</p>
        <p><strong>Successful:</strong> {summary.get('successful_tests', 0)}</p>
        <p><strong>Avg Compile Time:</strong> {summary.get('avg_compile_time_ms', 0):.2f} ms</p>
        <p><strong>Avg Exec Time:</strong> {summary.get('avg_exec_time_ms', 0):.4f} ms</p>
    </div>
'''
            
            # Detailed results
            html += '''
    <h2>Detailed Results</h2>
    <table>
        <tr><th>Test</th><th>-O0 (ms)</th><th>-O1 (ms)</th><th>-O2 (ms)</th><th>-O3 (ms)</th></tr>
'''
            for test in benchmark_data.get('tests', [])[:20]:
                name = test.get('name', 'unknown')
                times = []
                for opt in test.get('optimizations', []):
                    exec_time = opt.get('exec_avg_ms', 0) if opt.get('success') else 'N/A'
                    times.append(f'{exec_time:.4f}' if isinstance(exec_time, float) else exec_time)
                while len(times) < 4:
                    times.append('N/A')
                html += f'        <tr><td>{name}</td><td>{times[0]}</td><td>{times[1]}</td><td>{times[2]}</td><td>{times[3]}</td></tr>\n'
            html += '''    </table>
'''
        
        # Differential verification
        if diff_data:
            summary = diff_data.get('summary', {})
            total = summary.get('total', 0)
            passed = summary.get('passed', 0)
            pass_rate = (passed / total * 100) if total > 0 else 0
            
            html += f'''
    <h2>Differential Verification (vs GCC)</h2>
    <div class="summary">
        <p><strong>Total:</strong> {total}</p>
        <p><strong>Passed:</strong> <span class="pass">{passed}</span></p>
        <p><strong>Failed:</strong> <span class="fail">{summary.get('failed', 0)}</span></p>
        <p><strong>Pass Rate:</strong> {pass_rate:.1f}%</p>
    </div>
'''
        
        html += '''
</body>
</html>
'''
        
        # Save report
        output_file = self.output_dir / 'benchmark_report.html'
        with open(output_file, 'w') as f:
            f.write(html)
        
        return output_file
    
    def generate_json_summary(self, benchmark_data, diff_data=None, perf_data=None):
        """Generate JSON summary."""
        summary = {
            'timestamp': datetime.now().isoformat(),
            'benchmark': benchmark_data.get('summary', {}) if benchmark_data else {},
            'differential': diff_data.get('summary', {}) if diff_data else {},
            'performance': perf_data.get('summary', {}) if perf_data else {}
        }
        
        output_file = self.output_dir / 'summary.json'
        with open(output_file, 'w') as f:
            json.dump(summary, f, indent=2)
        
        return output_file
    
    def generate_all_reports(self, benchmark_file='benchmark_results.json',
                            diff_file='diff_verify_O2.json',
                            perf_file='perf_analysis_O2.json'):
        """Generate all reports."""
        print(f"\n{'='*60}")
        print(f"Generating Reports")
        print(f"{'='*60}\n")
        
        # Load data
        benchmark_data = self.load_results(benchmark_file)
        diff_data = self.load_results(diff_file)
        perf_data = self.load_results(perf_file)
        
        # Generate HTML report
        if benchmark_data or diff_data:
            html_file = self.generate_html_report(benchmark_data, diff_data, perf_data)
            print(f"  HTML report: {html_file}")
        
        # Generate JSON summary
        json_file = self.generate_json_summary(benchmark_data, diff_data, perf_data)
        print(f"  JSON summary: {json_file}")
        
        print(f"\n{'='*60}")
        print("Reports generated successfully!")
        print(f"{'='*60}\n")


def main():
    parser = argparse.ArgumentParser(description="Task 59: Report Generator")
    parser.add_argument('--results', default='benchmark_results', help='Results directory')
    parser.add_argument('--output', default='reports', help='Output directory')
    parser.add_argument('--benchmark', default='benchmark_results.json', help='Benchmark results file')
    parser.add_argument('--diff', default='diff_verify_O2.json', help='Differential results file')
    parser.add_argument('--perf', default='perf_analysis_O2.json', help='Performance results file')
    
    args = parser.parse_args()
    
    generator = ReportGenerator(args.results, args.output)
    generator.generate_all_reports(args.benchmark, args.diff, args.perf)


if __name__ == '__main__':
    main()
