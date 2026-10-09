#!/usr/bin/env python3
"""Quick test of stack alignment on all test files"""
import subprocess
import tempfile
import os
from pathlib import Path

test_dir = Path("test/jit")
total_issues = 0
total_tests = 0

for test_file in sorted(test_dir.glob("*.c")):
    total_tests += 1
    try:
        # Compile to assembly
        result = subprocess.run(
            ["./build/bin/zerocc", "-S", "-o", "-", str(test_file)],
            capture_output=True,
            text=True,
            timeout=5
        )
        
        if result.returncode != 0:
            # Skip tests that don't compile to assembly (JIT tests with internal code)
            continue
        
        # Write to temp file
        with tempfile.NamedTemporaryFile(mode='w', suffix='.s', delete=False) as f:
            f.write(result.stdout)
            temp_file = f.name
        
        # Check alignment
        result = subprocess.run(
            ["python3", "tools/stack_alignment_checker.py", "--asm-file", temp_file],
            capture_output=True,
            text=True,
            timeout=5
        )
        
        os.unlink(temp_file)
        
        if "Issues found: 0" in result.stdout:
            print(f"✅ {test_file.name}")
        else:
            print(f"❌ {test_file.name}")
            print(result.stdout)
            total_issues += 1
            
    except subprocess.TimeoutExpired:
        print(f"⏱️  {test_file.name} (timeout)")
        total_issues += 1
    except Exception as e:
        print(f"⚠️  {test_file.name} ({e})")

print(f"\n{total_tests - total_issues}/{total_tests} passed")
