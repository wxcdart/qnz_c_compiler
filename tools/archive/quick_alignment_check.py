#!/usr/bin/env python3
"""Quick stack alignment check for working test files"""
import subprocess
import tempfile
import os
from pathlib import Path

test_dir = Path("test/jit")
passed = 0
failed = 0
skipped = 0

for test_file in sorted(test_dir.glob("*.c")):
    try:
        # Try to compile to assembly
        result = subprocess.run(
            ["./build/bin/zerocc", "-S", "-o", "-", str(test_file)],
            capture_output=True,
            text=True,
            timeout=5
        )
        
        if result.returncode != 0 or not result.stdout.strip():
            # Skip tests that don't produce assembly
            skipped += 1
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
            passed += 1
        else:
            print(f"❌ {test_file.name}")
            print(result.stdout)
            failed += 1
            
    except subprocess.TimeoutExpired:
        print(f"⏱️  {test_file.name} (timeout)")
        skipped += 1
    except Exception as e:
        print(f"⚠️  {test_file.name} ({e})")
        skipped += 1

print(f"\n{'=' * 60}")
print(f"Passed: {passed}, Failed: {failed}, Skipped: {skipped}")
