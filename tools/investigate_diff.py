#!/usr/bin/env python3
"""
Task 59: Difference Investigation Tool
Investigates why ZeroCC and GCC produce different results.
"""

import os
import sys
import subprocess
import tempfile
from pathlib import Path

def get_zerocc_ir(source_file, opt_level="-O2"):
    """Get ZeroCC IR (placeholder - would need -dump-ir flag)."""
    # For now, just note that this would dump IR
    return None

def get_gcc_assembly(source_file, opt_level="-O2"):
    """Get GCC assembly output."""
    try:
        result = subprocess.run([
            'gcc', opt_level, '-S', '-fverbose-asm',
            source_file, '-o', '-'
        ], capture_output=True, timeout=30)
        
        if result.returncode == 0:
            return result.stdout.decode('utf-8')
        return None
    except Exception as e:
        return None

def get_zerocc_assembly(source_file, opt_level="-O2"):
    """Get ZeroCC assembly (placeholder - would need -S flag)."""
    # For now, just note that this would dump assembly
    return None

def trace_execution(source_file, opt_level="-O2"):
    """Trace execution with strace."""
    try:
        # Compile with GCC
        subprocess.run([
            'gcc', opt_level, source_file, '-o', '/tmp/trace_test'
        ], capture_output=True, timeout=30)
        
        # Run with strace
        result = subprocess.run([
            'strace', '-e', 'trace=none', '/tmp/trace_test'
        ], capture_output=True, timeout=5)
        
        return result.stderr.decode('utf-8', errors='replace')
    except Exception as e:
        return f"Error: {e}"

def analyze_call_heavy(source_file):
    """Analyze call_heavy.c specifically."""
    print(f"\n{'='*60}")
    print(f"Analyzing: {source_file}")
    print(f"{'='*60}")
    
    # Get GCC assembly
    gcc_asm = get_gcc_assembly(source_file, "-O2")
    if gcc_asm:
        print("\nGCC Assembly (first 50 lines):")
        print("-" * 40)
        lines = gcc_asm.split('\n')[:50]
        for line in lines:
            print(line)
    
    # Look for inline function patterns
    if gcc_asm:
        print("\nInlining Analysis:")
        print("-" * 40)
        if 'square' in gcc_asm:
            print("  - 'square' function found in assembly")
        if 'cube' in gcc_asm:
            print("  - 'cube' function found in assembly")
        if 'inline' in gcc_asm.lower():
            print("  - Inline hints detected")

def analyze_memory_heavy(source_file):
    """Analyze memory_heavy.c specifically."""
    print(f"\n{'='*60}")
    print(f"Analyzing: {source_file}")
    print(f"{'='*60}")
    
    # Get GCC assembly
    gcc_asm = get_gcc_assembly(source_file, "-O2")
    if gcc_asm:
        print("\nGCC Assembly (first 50 lines):")
        print("-" * 40)
        lines = gcc_asm.split('\n')[:50]
        for line in lines:
            print(line)
        
        # Look for memory access patterns
        print("\nMemory Access Analysis:")
        print("-" * 40)
        if 'mov' in gcc_asm:
            mov_count = gcc_asm.count('mov')
            print(f"  - {mov_count} memory operations detected")
        if 'lea' in gcc_asm:
            lea_count = gcc_asm.count('lea')
            print(f"  - {lea_count} address calculations detected")

def compare_bytecode(source_file):
    """Compare compiled bytecode (placeholder for future)."""
    print(f"\nBytecode Comparison (placeholder):")
    print("-" * 40)
    print("  - Would compare ZeroCC IR with GCC IR")
    print("  - Requires -dump-ir and GCC -fdump-ir flags")

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 tools/investigate_diff.py <test_file>")
        print("\nExamples:")
        print("  python3 tools/investigate_diff.py test/bench/call_heavy.c")
        print("  python3 tools/investigate_diff.py test/bench/memory_heavy.c")
        sys.exit(1)
    
    source_file = sys.argv[1]
    if not os.path.exists(source_file):
        print(f"Error: {source_file} not found")
        sys.exit(1)
    
    filename = Path(source_file).name
    
    if 'call_heavy' in filename:
        analyze_call_heavy(source_file)
    elif 'memory_heavy' in filename:
        analyze_memory_heavy(source_file)
    else:
        # Generic analysis
        print(f"\n{'='*60}")
        print(f"Generic Analysis: {source_file}")
        print(f"{'='*60}")
        
        gcc_asm = get_gcc_assembly(source_file, "-O2")
        if gcc_asm:
            print("\nGCC Assembly (first 30 lines):")
            print("-" * 40)
            for line in gcc_asm.split('\n')[:30]:
                print(line)
        
        compare_bytecode(source_file)
    
    print(f"\n{'='*60}")
    print("Next Steps:")
    print(f"{'='*60}")
    print("1. Add -dump-ir flag to ZeroCC to see IR")
    print("2. Add -S flag to ZeroCC to see assembly")
    print("3. Compare register allocation patterns")
    print("4. Check function inlining decisions")
    print("5. Verify memory access ordering")
    print(f"{'='*60}\n")

if __name__ == '__main__':
    main()
