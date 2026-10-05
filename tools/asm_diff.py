#!/usr/bin/env python3
"""
Task 59: Assembly Comparison Tool
Compares ZeroCC and GCC assembly output to identify differences.
"""

import os
import sys
import subprocess
from pathlib import Path

def get_gcc_assembly(source_file, opt_level="-O2", flags=None):
    """Get GCC assembly output."""
    extra_flags = flags or []
    cmd = ['gcc', opt_level, '-S'] + extra_flags + [source_file, '-o', '-']
    
    try:
        result = subprocess.run(cmd, capture_output=True, timeout=30)
        if result.returncode == 0:
            return result.stdout.decode('utf-8')
        return None
    except Exception as e:
        return None

def analyze_assembly(asm_text, name):
    """Analyze assembly for key patterns."""
    print(f"\n{name} Assembly Analysis:")
    print("=" * 60)
    
    # Count instruction types
    patterns = {
        'mov': asm_text.count('\tmov'),
        'call': asm_text.count('\tcall'),
        'jmp': asm_text.count('\tjmp'),
        'add': asm_text.count('\tadd'),
        'sub': asm_text.count('\tsub'),
        'imul': asm_text.count('\timul'),
        'idiv': asm_text.count('\tidiv'),
        'push': asm_text.count('\tpush'),
        'pop': asm_text.count('\tpop'),
        'lea': asm_text.count('\tlea'),
        'xmm': asm_text.count('%xmm'),  # SIMD
        'ymm': asm_text.count('%ymm'),  # AVX
        'stack': asm_text.count('%rsp'),
    }
    
    print("Instruction counts:")
    for pattern, count in patterns.items():
        if count > 0:
            print(f"  {pattern:10s}: {count}")
    
    # Check for specific features
    print("\nFeature detection:")
    if 'xmm' in asm_text or 'ymm' in asm_text:
        print("  ✅ SIMD/Vectorization detected")
    else:
        print("  ❌ No SIMD/Vectorization")
    
    if 'stack protector' in asm_text.lower() or '%fs:40' in asm_text:
        print("  ✅ Stack canary detected")
    else:
        print("  ❌ No stack canary")
    
    if 'endbr64' in asm_text:
        print("  ✅ CET (Control-flow Enforcement) detected")
    else:
        print("  ❌ No CET")
    
    # Find function calls
    calls = [line.strip() for line in asm_text.split('\n') if '\tcall' in line and '@' not in line]
    if calls:
        print(f"\nFunction calls ({len(calls)}):")
        for call in calls[:5]:
            print(f"  {call}")

def compare_test(source_file):
    """Compare GCC assembly for a test file."""
    filename = Path(source_file).name
    print(f"\n{'='*60}")
    print(f"Assembly Analysis: {filename}")
    print(f"{'='*60}")
    
    # Get GCC assembly with different flags
    asm_opt0 = get_gcc_assembly(source_file, "-O0")
    asm_opt2 = get_gcc_assembly(source_file, "-O2")
    asm_opt3 = get_gcc_assembly(source_file, "-O3")
    
    if asm_opt0:
        analyze_assembly(asm_opt0, "GCC -O0")
    
    if asm_opt2:
        analyze_assembly(asm_opt2, "GCC -O2")
    
    if asm_opt3:
        analyze_assembly(asm_opt3, "GCC -O3")
    
    # Compare optimization effects
    if asm_opt0 and asm_opt2:
        print(f"\nOptimization Impact (-O0 vs -O2):")
        print("-" * 60)
        lines_opt0 = len(asm_opt0.split('\n'))
        lines_opt2 = len(asm_opt2.split('\n'))
        print(f"  Lines: {lines_opt0} → {lines_opt2} ({'+' if lines_opt2 > lines_opt0 else ''}{lines_opt2 - lines_opt0})")
        
        calls_opt0 = asm_opt0.count('\tcall')
        calls_opt2 = asm_opt2.count('\tcall')
        print(f"  Calls: {calls_opt0} → {calls_opt2} ({'+' if calls_opt2 > calls_opt0 else ''}{calls_opt2 - calls_opt0})")
        
        simd_opt0 = asm_opt0.count('%xmm')
        simd_opt2 = asm_opt2.count('%xmm')
        print(f"  SIMD: {simd_opt0} → {simd_opt2} ({'+' if simd_opt2 > simd_opt0 else ''}{simd_opt2 - simd_opt0})")

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 tools/asm_diff.py <test_file>")
        print("\nExamples:")
        print("  python3 tools/asm_diff.py test/bench/call_heavy.c")
        print("  python3 tools/asm_diff.py test/bench/memory_heavy.c")
        sys.exit(1)
    
    source_file = sys.argv[1]
    if not os.path.exists(source_file):
        print(f"Error: {source_file} not found")
        sys.exit(1)
    
    compare_test(source_file)
    
    print(f"\n{'='*60}")
    print("Recommendations:")
    print(f"{'='*60}")
    print("1. ZeroCC doesn't support SIMD - expect scalar code")
    print("2. ZeroCC doesn't do aggressive inlining like GCC")
    print("3. ZeroCC doesn't use stack canaries")
    print("4. Focus on semantic equivalence, not instruction matching")
    print(f"{'='*60}\n")

if __name__ == '__main__':
    main()
