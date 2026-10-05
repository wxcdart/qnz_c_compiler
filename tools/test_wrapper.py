#!/usr/bin/env python3
"""
Run a single test with fresh compilation.
Usage: python3 tools/test_wrapper.py <category>/<test_name>
Examples:
  python3 tools/test_wrapper.py jit/vla_test
  python3 tools/test_wrapper.py unit/ir_test
  python3 tools/test_wrapper.py abi/abi_sum_jit
"""

import sys
import subprocess
import os
from pathlib import Path

def main():
    if len(sys.argv) != 2:
        print("Usage: python3 tools/test_wrapper.py <category>/<test_name>")
        print("Examples:")
        print("  python3 tools/test_wrapper.py jit/vla_test")
        print("  python3 tools/test_wrapper.py unit/ir_test")
        print("  python3 tools/test_wrapper.py abi/abi_sum_jim")
        sys.exit(1)
    
    test_ref = sys.argv[1]
    
    if '/' not in test_ref:
        print(f"Error: Invalid format. Use <category>/<test_name>")
        sys.exit(1)
    
    category, test_name = test_ref.split('/', 1)
    test_src = Path(f"test/{category}/{test_name}.c")
    
    if not test_src.exists():
        print(f"Error: Test not found: {test_src}")
        sys.exit(1)
    
    test_bin = Path(f"build/test/{category}/{test_name}")
    
    # Build the test
    print(f"Building test: {category}/{test_name}")
    sys.stdout.flush()
    result = subprocess.run(
        ["make", str(test_bin)],
        capture_output=True,
        text=True
    )
    
    if result.returncode != 0:
        print(f"Build failed:\n{result.stderr}")
        sys.exit(1)
    
    # Run the test
    print(f"Running test: {category}/{test_name}")
    sys.stdout.flush()
    result = subprocess.run(
        [str(test_bin)]
    )
    
    sys.exit(result.returncode)

if __name__ == "__main__":
    main()
