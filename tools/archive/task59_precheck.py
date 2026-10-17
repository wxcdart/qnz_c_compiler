#!/usr/bin/env python3
"""
Task 59 Pre-Check: Optimization Benchmarking Readiness
Checks if the infrastructure for Task 59 (benchmarking & differential verification) is ready.
"""

import json
import sys
import os
import subprocess
from pathlib import Path

def load_deps(filepath):
    with open(filepath, 'r') as f:
        return json.load(f)

def check_opt_passes(deps):
    """Check if all optimization passes are implemented."""
    files = deps.get('files', {})
    
    required_files = [
        'zero_cc/parser/opt_pass.h',
        'zero_cc/parser/opt_pass.c',
        'zero_cc/parser/opt_constant_fold.h',
        'zero_cc/parser/opt_constant_fold.c',
        'zero_cc/parser/opt_dce.h',
        'zero_cc/parser/opt_dce.c',
        'zero_cc/parser/opt_cse.h',
        'zero_cc/parser/opt_cse.c',
        'zero_cc/parser/opt_loop.h',
        'zero_cc/parser/opt_loop.c',
    ]
    
    missing = []
    for f in required_files:
        if f not in files:
            missing.append(f)
    
    return len(missing) == 0, missing

def check_benchmark_infrastructure():
    """Check if benchmarking infrastructure exists."""
    # Check for common benchmark tools
    tools = {
        'perf': subprocess.run(['which', 'perf'], capture_output=True).returncode == 0,
        'valgrind': subprocess.run(['which', 'valgrind'], capture_output=True).returncode == 0,
        'gcc': subprocess.run(['which', 'gcc'], capture_output=True).returncode == 0,
    }
    
    return tools

def check_test_suite():
    """Check if test suite is comprehensive enough."""
    # Count actual test files in test/ directory
    test_dir = Path('test')
    if not test_dir.exists():
        return False, 0
    
    test_files = list(test_dir.glob("**/*.c"))
    return len(test_files) >= 50, len(test_files)

def check_ir_count(deps):
    """Check if IR has enough instructions for meaningful benchmarks."""
    functions = deps.get('functions', {})
    
    ir_funcs = [f for f in functions.keys() if f.startswith('ir_')]
    
    return len(ir_funcs) > 50, len(ir_funcs)

def main():
    print("=" * 60)
    print("Task 59 Pre-Check: Optimization Benchmarking Readiness")
    print("=" * 60)
    print()
    
    if len(sys.argv) < 2:
        print("Usage: python3 tools/task59_precheck.py <deps_graph.json>")
        sys.exit(1)
    
    deps_file = sys.argv[1]
    if not os.path.exists(deps_file):
        print(f"Error: {deps_file} not found")
        sys.exit(1)
    
    deps = load_deps(deps_file)
    
    checks = [
        ("Optimization passes implemented", check_opt_passes(deps)),
        ("IR infrastructure", check_ir_count(deps)),
        ("Test suite size (50+ tests)", check_test_suite()),
    ]
    
    all_passed = True
    for name, (passed, info) in checks:
        status = "✅ PASS" if passed else "❌ FAIL"
        print(f"{status}: {name}")
        if isinstance(info, int):
            print(f"  - Count: {info}")
        elif isinstance(info, list) and len(info) > 0:
            for i in info[:5]:
                print(f"  - {i}")
        if not passed:
            all_passed = False
    
    print()
    print("Benchmarking tools available:")
    print("-" * 40)
    tools = check_benchmark_infrastructure()
    for tool, available in tools.items():
        status = "✅" if available else "❌"
        print(f"{status} {tool}")
    
    print()
    print("=" * 60)
    if all_passed:
        print("✅ All checks passed! Ready for Task 59")
        print()
        print("Task 59 will implement:")
        print("  1. Compilation speed benchmarking (vs GCC)")
        print("  2. Execution throughput benchmarking (vs GCC)")
        print("  3. Differential verification against GCC")
        print("  4. Optimization level comparison (-O0 to -O3)")
        print()
        print("Next steps:")
        print("  1. Create benchmark harness (tools/benchmark_suite.py)")
        print("  2. Create test suite expansion (test/bench/...)")
        print("  3. Implement timing infrastructure")
        print("  4. Run differential verification against GCC")
        print("  5. Generate benchmark reports")
        sys.exit(0)
    else:
        print("❌ Some checks failed. Fix before Task 59")
        sys.exit(1)

if __name__ == '__main__':
    main()
