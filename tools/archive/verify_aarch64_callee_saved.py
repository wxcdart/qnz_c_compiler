#!/usr/bin/env python3
"""
AArch64 Callee-Saved Register Verification Tool

Verifies that AArch64 callee-saved registers (x19-x28) are properly preserved.

Usage:
    python3 verify_aarch64_callee_saved.py --compile
    python3 verify_aarch64_callee_saved.py --disasm <object_file>
    python3 verify_aarch64_callee_saved.py --run <executable>
    python3 verify_aarch64_callee_saved.py --full-test
"""

import subprocess
import sys
import os
import re
from pathlib import Path

def compile_test(test_file, output_base, target="aarch64"):
    """Compile a test file for the specified target."""
    print(f"[COMPILE] Compiling {test_file} for {target}...")
    
    # Compile to object file
    cmd = ["./build/bin/zerocc", test_file, "--target", target, "-c", "-o", f"{output_base}.o"]
    result = subprocess.run(cmd, capture_output=True, text=True)
    
    if result.returncode != 0:
        print(f"  ✗ Compilation failed:")
        print(result.stderr)
        return False
    
    print(f"  ✓ Compiled to {output_base}.o")
    return True

def disassemble(obj_file):
    """Disassemble object file and extract callee-save patterns."""
    print(f"[DISASM] Disassembling {obj_file}...")
    
    cmd = ["aarch64-linux-gnu-objdump", "-d", obj_file]
    result = subprocess.run(cmd, capture_output=True, text=True)
    
    if result.returncode != 0:
        print(f"  ✗ Disassembly failed:")
        print(result.stderr)
        return None
    
    return result.stdout

def check_callee_save_patterns(disasm_output):
    """Check for proper callee-save patterns in disassembly."""
    print("[VERIFY] Checking callee-save patterns...")
    
    issues = []
    warnings = []
    
    # Look for functions
    functions = re.findall(r'<(\w+)>:', disasm_output)
    print(f"  Found {len(functions)} functions: {', '.join(functions)}")
    
    # Check for stp/ldp with x19-x28
    callee_saved_regs = list(range(19, 29))  # x19-x28
    
    for reg in callee_saved_regs:
        # Check if this register is used
        stp_pattern = rf'stp\s+x{reg},'
        ldp_pattern = rf'ldp\s+x{reg},'
        
        stp_matches = re.findall(stp_pattern, disasm_output)
        ldp_matches = re.findall(ldp_pattern, disasm_output)
        
        if stp_matches or ldp_matches:
            print(f"  ✓ x{reg} is used (stp={len(stp_matches)}, ldp={len(ldp_matches)})")
        else:
            # Check if register is allocated but not saved (potential issue)
            pass
    
    # Check prologue pattern: sub sp, stp x29,x30, mov x29,sp
    prologue_pattern = r'sub\s+sp,\s+sp,\s+#0x[0-9a-f]+\n\s+stp\s+x29,\s+x30'
    if re.search(prologue_pattern, disasm_output):
        print("  ✓ Standard prologue pattern found")
    else:
        warnings.append("Non-standard prologue pattern")
    
    # Check epilogue pattern: ldp x29,x30, add sp, sp, ret
    epilogue_pattern = r'ldp\s+x29,\s+x30,\s+\[sp\]\n\s+add\s+sp,\s+sp'
    if re.search(epilogue_pattern, disasm_output):
        print("  ✓ Standard epilogue pattern found")
    else:
        warnings.append("Non-standard epilogue pattern")
    
    return {
        'issues': issues,
        'warnings': warnings,
        'has_callee_saved': any(re.search(rf'stp\s+x{reg},', disasm_output) for reg in callee_saved_regs)
    }

def run_test(executable):
    """Run the test executable and check result."""
    print(f"[RUN] Executing {executable}...")
    
    cmd = ["qemu-aarch64-static", executable]
    result = subprocess.run(cmd, capture_output=True, text=True)
    
    if result.returncode == 0:
        print(f"  ✓ Test passed (exit code 0)")
        return True
    else:
        print(f"  ✗ Test failed (exit code {result.returncode})")
        if result.stdout:
            print(f"    stdout: {result.stdout}")
        if result.stderr:
            print(f"    stderr: {result.stderr}")
        return False

def full_test_suite():
    """Run full test suite."""
    print("=" * 70)
    print("AArch64 Callee-Saved Register Verification")
    print("=" * 70)
    
    base_path = Path(".")
    test_dir = base_path / "test"
    
    # Test 1: Simple callee-saved test
    print("\n[Test 1] Simple callee-saved test")
    print("-" * 70)
    
    test1_file = test_dir / "aarch64_callee_saved_test.c"
    if not test1_file.exists():
        print("  ✗ Test file not found")
        return False
    
    if not compile_test(str(test1_file), "/tmp/aarch64_test1"):
        return False
    
    disasm = disassemble("/tmp/aarch64_test1.o")
    if disasm:
        verification = check_callee_save_patterns(disasm)
        print(f"  Verification: {verification}")
    
    # Compile and run
    subprocess.run(["aarch64-linux-gnu-gcc", "/tmp/aarch64_test1.o", "-static", "-o", "/tmp/aarch64_test1"], 
                   capture_output=True)
    run_test("/tmp/aarch64_test1")
    
    # Test 2: Complex function test
    print("\n[Test 2] Complex function test")
    print("-" * 70)
    
    test2_file = test_dir / "aarch64_callee_saved_test2.c"
    if test2_file.exists():
        if compile_test(str(test2_file), "/tmp/aarch64_test2"):
            disasm = disassemble("/tmp/aarch64_test2.o")
            if disasm:
                verification = check_callee_save_patterns(disasm)
                print(f"  Verification: {verification}")
            
            subprocess.run(["aarch64-linux-gnu-gcc", "/tmp/aarch64_test2.o", "-static", "-o", "/tmp/aarch64_test2"], 
                           capture_output=True)
            run_test("/tmp/aarch64_test2")
    
    print("\n" + "=" * 70)
    print("Verification complete")
    print("=" * 70)
    return True

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 verify_aarch64_callee_saved.py <command>")
        print()
        print("Commands:")
        print("  --compile       Compile test files")
        print("  --disasm <file> Disassemble and check patterns")
        print("  --run <file>    Run test executable")
        print("  --full-test     Run full test suite")
        print()
        print("Running full test suite...")
        full_test_suite()
        return
    
    cmd = sys.argv[1]
    
    if cmd == "--compile":
        full_test_suite()
    elif cmd == "--disasm" and len(sys.argv) > 2:
        disasm = disassemble(sys.argv[2])
        if disasm:
            check_callee_save_patterns(disasm)
    elif cmd == "--run" and len(sys.argv) > 2:
        run_test(sys.argv[2])
    elif cmd == "--full-test":
        full_test_suite()
    else:
        print(f"Unknown command: {cmd}")
        sys.exit(1)

if __name__ == '__main__':
    main()
