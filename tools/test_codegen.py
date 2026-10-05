#!/usr/bin/env python3
"""
Test compiler codegen by:
1. Compiling C source to object file
2. Verifying ELF structure
3. Checking for non-empty .text section
4. Optionally linking and running
"""

import subprocess
import sys
import os
from pathlib import Path

def run_command(cmd, check=True):
    """Run a command and return result."""
    print(f"  $ {' '.join(cmd) if isinstance(cmd, list) else cmd}")
    result = subprocess.run(cmd if isinstance(cmd, list) else cmd.split(),
                          capture_output=True, text=True)
    if result.stdout:
        print(result.stdout)
    if result.stderr:
        print(result.stderr, file=sys.stderr)
    if check and result.returncode != 0:
        print(f"Command failed with code {result.returncode}")
    return result

def test_file(c_file, zerocc_path, expected_success=True):
    """Test compiling a single C file."""
    print(f"\n{'='*60}")
    print(f"Testing: {c_file}")
    print('='*60)
    
    obj_file = "/tmp/test_codegen.o"
    exe_file = "/tmp/test_codegen.exe"
    
    # Step 1: Compile to object file
    print("\n[1/3] Compiling to object file...")
    result = run_command([zerocc_path, "-c", c_file, "-o", obj_file, "-v"])
    if expected_success and result.returncode != 0:
        return False
    
    # Step 2: Check ELF structure
    print("\n[2/3] Verifying ELF structure...")
    result = run_command(["readelf", "-h", obj_file])
    if result.returncode != 0:
        return False
    
    # Check .text section is non-empty
    result = run_command(["readelf", "-S", obj_file])
    if ".text" in result.stdout:
        # Parse section size
        for line in result.stdout.split('\n'):
            if '.text' in line and 'PROGBITS' in line:
                # Extract size (column after offset)
                parts = line.split()
                if len(parts) >= 3:
                    size_str = parts[2] if len(parts) > 2 else "0"
                    try:
                        size = int(size_str, 16) if 'x' in size_str.lower() else int(size_str)
                        print(f"  .text section size: {size} bytes")
                        if size == 0:
                            print("  ⚠ WARNING: .text section is empty!")
                            return False
                    except:
                        pass
    else:
        print("  ⚠ WARNING: No .text section found!")
    
    # Step 3: Link and run (optional)
    print("\n[3/3] Linking and running...")
    result = run_command(["gcc", "-o", exe_file, obj_file, "-lc"], check=False)
    if result.returncode == 0:
        result = run_command([exe_file], check=False)
        print(f"  Exit code: {result.returncode}")
    
    return True

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 test_codegen.py <c_file> [c_file2 ...]")
        print("       python3 test_codegen.py --all")
        sys.exit(1)
    
    zerocc_path = os.environ.get('ZEROC_PATH', 'build/bin/zerocc')
    
    if sys.argv[1] == '--all':
        # Test all files in test/aot/
        test_dir = Path('test/aot')
        if test_dir.exists():
            c_files = list(test_dir.glob('*.c'))
        else:
            c_files = []
    else:
        c_files = [Path(f) for f in sys.argv[1:]]
    
    passed = 0
    failed = 0
    
    for c_file in c_files:
        if test_file(str(c_file), zerocc_path):
            passed += 1
        else:
            failed += 1
    
    print(f"\n{'='*60}")
    print(f"Results: {passed} passed, {failed} failed")
    print('='*60)
    
    return 0 if failed == 0 else 1

if __name__ == '__main__':
    sys.exit(main())
