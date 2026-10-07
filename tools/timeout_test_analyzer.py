#!/usr/bin/env python3
"""
Timeout Test Analyzer

Analyzes tests that are timing out to help identify the root cause:
- Infinite loops
- Memory leaks  
- Performance issues
- Deadlocks

Usage:
    python3 tools/timeout_test_analyzer.py [--test TEST_NAME] [--verbose]
"""

import subprocess
import sys
import os
import time
import re
import signal
import resource
from pathlib import Path

# Tests that are timing out
TIMEOUT_TESTS = [
    'regalloc_test',
    'vla_test',
    'jit/regalloc_test',
    'jit/vla_test',
]

class TimeoutTestAnalyzer:
    def __init__(self, verbose=False):
        self.verbose = verbose
        self.compiler_path = './build/bin/zerocc'
        
    def run_test_with_timeout(self, test_name, timeout=5.0):
        """Run a test with timeout and capture output"""
        test_path = f'test/jit/{test_name}.c' if not test_name.startswith('jit/') else f'test/{test_name}.c'
        
        if not os.path.exists(test_path):
            print(f"[ERROR] Test file not found: {test_path}")
            return None
        
        # Create temporary output file
        import tempfile
        with tempfile.NamedTemporaryFile(mode='w', suffix='.c', delete=False) as f:
            with open(test_path, 'r') as src:
                content = src.read()
                f.write(content)
            temp_src = f.name
        
        temp_obj = temp_src.replace('.c', '.o')
        temp_exe = temp_src.replace('.c', '')
        
        try:
            # Step 1: Compile
            start = time.time()
            result = subprocess.run(
                [self.compiler_path, temp_src, '-o', temp_obj, '-c'],
                capture_output=True,
                text=True,
                timeout=30.0
            )
            compile_time = time.time() - start
            
            if result.returncode != 0:
                print(f"[ERROR] Compilation failed for {test_name}")
                print(f"  stderr: {result.stderr[:500]}")
                return {'status': 'compile_fail', 'compile_time': compile_time}
            
            # Step 2: Link
            start = time.time()
            result = subprocess.run(
                ['gcc', temp_obj, '-static', '-o', temp_exe],
                capture_output=True,
                text=True,
                timeout=30.0
            )
            link_time = time.time() - start
            
            if result.returncode != 0:
                print(f"[ERROR] Linking failed for {test_name}")
                return {'status': 'link_fail', 'compile_time': compile_time, 'link_time': link_time}
            
            # Step 3: Run with timeout
            start = time.time()
            try:
                result = subprocess.run(
                    [temp_exe],
                    capture_output=True,
                    text=True,
                    timeout=timeout
                )
                run_time = time.time() - start
                
                return {
                    'status': 'success' if result.returncode == 0 else 'runtime_error',
                    'returncode': result.returncode,
                    'compile_time': compile_time,
                    'link_time': link_time,
                    'run_time': run_time,
                    'stdout': result.stdout[:200] if result.stdout else '',
                    'stderr': result.stderr[:200] if result.stderr else ''
                }
            except subprocess.TimeoutExpired:
                run_time = time.time() - start
                return {
                    'status': 'timeout',
                    'compile_time': compile_time,
                    'link_time': link_time,
                    'run_time': run_time,
                }
                
        finally:
            # Cleanup
            for f in [temp_src, temp_obj, temp_exe]:
                if os.path.exists(f):
                    os.unlink(f)
    
    def analyze_test(self, test_name):
        """Analyze a single test"""
        print(f"\n{'='*60}")
        print(f"Analyzing: {test_name}")
        print('='*60)
        
        result = self.run_test_with_timeout(test_name, timeout=5.0)
        
        if not result:
            return None
        
        print(f"Status: {result['status']}")
        print(f"Compile time: {result.get('compile_time', 0):.3f}s")
        print(f"Link time: {result.get('link_time', 0):.3f}s")
        print(f"Run time: {result.get('run_time', 0):.3f}s")
        
        if result['status'] == 'timeout':
            print("\n[DIAGNOSIS] Test timed out after 5 seconds")
            print("Possible causes:")
            print("  1. Infinite loop in generated code")
            print("  2. Very slow algorithm (exponential complexity)")
            print("  3. Deadlock or resource contention")
            print("\nRecommendations:")
            print("  - Run with gdb to find where it hangs")
            print("  - Check for infinite loops in the test code")
            print("  - Profile with perf or valgrind")
            
        elif result['status'] == 'runtime_error':
            print(f"\n[DIAGNOSIS] Runtime error, return code: {result['returncode']}")
            if result.get('stderr'):
                print(f"  stderr: {result['stderr']}")
                
        elif result['status'] == 'success':
            print(f"\n[DIAGNOSIS] Test passed, return code: {result['returncode']}")
            
        return result
    
    def compare_with_working_test(self, failing_test, working_test='simple_return'):
        """Compare a failing test with a working test"""
        print(f"\n{'='*60}")
        print(f"Comparing: {failing_test} vs {working_test}")
        print('='*60)
        
        failing = self.analyze_test(failing_test)
        working = self.analyze_test(working_test)
        
        if not failing or not working:
            return
        
        print(f"\nTime comparison:")
        print(f"  {failing_test}: compile={failing.get('compile_time', 0):.3f}s, "
              f"link={failing.get('link_time', 0):.3f}s, "
              f"run={failing.get('run_time', 0):.3f}s")
        print(f"  {working_test}: compile={working.get('compile_time', 0):.3f}s, "
              f"link={working.get('link_time', 0):.3f}s, "
              f"run={working.get('run_time', 0):.3f}s")
        
        if failing.get('compile_time', 0) > working.get('compile_time', 0) * 10:
            print("\n[WARNING] Compile time is significantly higher!")
        if failing.get('status') == 'timeout' and working.get('status') == 'success':
            print("\n[DIAGNOSIS] Working test completes, failing test hangs")
            print("This suggests an infinite loop or deadlock in the generated code")
    
    def run_all_timeout_tests(self):
        """Run all known timeout tests"""
        print(f"\n{'#'*70}")
        print(f"# Timeout Test Analysis")
        print(f"#{'#'*70}")
        
        results = {}
        for test in TIMEOUT_TESTS:
            # Normalize test name
            if test.startswith('jit/'):
                test_name = test[4:]  # Remove 'jit/' prefix
            else:
                test_name = test
            
            results[test] = self.analyze_test(test_name)
        
        # Summary
        print(f"\n{'='*70}")
        print("SUMMARY")
        print('='*70)
        
        timeouts = [t for t, r in results.items() if r and r['status'] == 'timeout']
        failures = [t for t, r in results.items() if r and r['status'] in ['runtime_error', 'compile_fail', 'link_fail']]
        successes = [t for t, r in results.items() if r and r['status'] == 'success']
        
        print(f"Timeouts: {len(timeouts)}")
        for t in timeouts:
            print(f"  - {t}")
        
        print(f"\nFailures: {len(failures)}")
        for t in failures:
            print(f"  - {t}")
        
        print(f"\nSuccesses: {len(successes)}")
        for t in successes:
            print(f"  - {t}")
        
        if timeouts:
            print(f"\n[RECOMMENDATION] {len(timeouts)} test(s) are timing out")
            print("Run with gdb to find the hang location:")
            for t in timeouts:
                print(f"  gdb --args ./build/bin/zerocc test/jit/{t}.c -o /tmp/test.o -c")
        
        return results


def main():
    import argparse
    parser = argparse.ArgumentParser(description='Analyze timeout test failures')
    parser.add_argument('--test', help='Specific test to analyze')
    parser.add_argument('--verbose', action='store_true', help='Verbose output')
    parser.add_argument('--compare', help='Compare with a working test')
    
    args = parser.parse_args()
    
    os.chdir(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    
    analyzer = TimeoutTestAnalyzer(verbose=args.verbose)
    
    if args.test:
        analyzer.analyze_test(args.test)
        if args.compare:
            analyzer.compare_with_working_test(args.test, args.compare)
    else:
        analyzer.run_all_timeout_tests()


if __name__ == '__main__':
    main()
