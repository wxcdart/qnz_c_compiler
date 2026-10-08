#!/usr/bin/env python3
"""
Assembly Emitter Validator Tool
Validates and compares assembly output from ZeroCC -S flag.

This tool helps verify that the -S flag correctly emits text assembly
instead of binary ELF output.
"""

import os
import sys
import subprocess
import re
from pathlib import Path

def run_zerocc_asm(source_file, opt_level="-O3", output_file=None):
    """Run ZeroCC with -S flag to generate assembly."""
    cmd = ['./build/bin/zerocc', '-S', opt_level, source_file]
    if output_file:
        cmd.extend(['-o', output_file])
    
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        return result.returncode, result.stdout, result.stderr
    except Exception as e:
        return -1, "", str(e)

def check_assembly_format(asm_content, filename):
    """Check if content is valid assembly text."""
    issues = []
    
    # Check for binary content (ELF magic bytes)
    if asm_content.startswith('\x7fELF') or asm_content[:4] == b'\x7fELF':
        issues.append("❌ OUTPUT IS BINARY ELF (not text assembly)")
        return issues
    
    # Check for valid assembly patterns
    lines = asm_content.split('\n')
    
    # Look for common assembly patterns
    has_section = any('.text' in line or '.data' in line or '.rodata' in line for line in lines)
    has_function = any(re.search(r'^\s*\.\s*globl\s+\w+', line) or re.search(r'^\s*\w+:', line) for line in lines)
    has_instructions = any(re.search(r'\t(mov|call|jmp|add|sub|push|pop|lea|cmp|test)\s', line) for line in lines)
    has_labels = any(re.search(r'^\s*[A-Za-z_][A-Za-z0-9_]*:', line) for line in lines)
    
    if not has_section:
        issues.append("⚠️  No .text/.data/.rodata sections found")
    if not has_function:
        issues.append("⚠️  No function declarations found")
    if not has_instructions:
        issues.append("⚠️  No instructions found")
    if not has_labels:
        issues.append("⚠️  No labels found")
    
    if not issues:
        issues.append("✅ Valid assembly format detected")
    
    return issues

def analyze_assembly_stats(asm_content):
    """Analyze assembly statistics."""
    lines = asm_content.split('\n')
    
    stats = {
        'total_lines': len(lines),
        'code_lines': 0,
        'comment_lines': 0,
        'blank_lines': 0,
        'instruction_counts': {},
        'functions': [],
        'simd_instructions': 0,
    }
    
    instruction_patterns = {
        'mov': r'\t(mov[qlwd]?|mov[sz]x|movabs)\s',
        'call': r'\tcall\s',
        'jmp': r'\t(jmp|je|jne|jl|jle|jg|jge|ja|jb)\s',
        'add': r'\tadd\s',
        'sub': r'\tsub\s',
        'imul': r'\timul\s',
        'idiv': r'\tidiv\s',
        'push': r'\tpush\s',
        'pop': r'\tpop\s',
        'lea': r'\tlea\s',
        'cmp': r'\tcmp\s',
        'test': r'\ttest\s',
        'xmm': r'\t(addss|subss|mulss|divss|movss|movaps|movapd|cvtsi2ss|cvtss2si)\s',
    }
    
    for line in lines:
        stripped = line.strip()
        
        if not stripped:
            stats['blank_lines'] += 1
        elif stripped.startswith('#') or stripped.startswith('//'):
            stats['comment_lines'] += 1
        elif not stripped.startswith('.'):
            stats['code_lines'] += 1
            
            # Count instructions
            for name, pattern in instruction_patterns.items():
                if re.search(pattern, line):
                    stats['instruction_counts'][name] = stats['instruction_counts'].get(name, 0) + 1
            
            # Count SIMD
            if re.search(instruction_patterns['xmm'], line):
                stats['simd_instructions'] += 1
        
        # Find functions
        func_match = re.match(r'^\s*\.globl\s+(\w+)', line)
        if func_match:
            stats['functions'].append(func_match.group(1))
    
    return stats

def print_assembly_report(source_file, asm_content, stats):
    """Print analysis report."""
    print(f"\n{'='*70}")
    print(f"Assembly Analysis: {Path(source_file).name}")
    print(f"{'='*70}")
    
    print(f"\n📊 Statistics:")
    print(f"  Total lines:     {stats['total_lines']}")
    print(f"  Code lines:      {stats['code_lines']}")
    print(f"  Comment lines:   {stats['comment_lines']}")
    print(f"  Blank lines:     {stats['blank_lines']}")
    print(f"  Functions:       {len(stats['functions'])} ({', '.join(stats['functions']) if stats['functions'] else 'none'})")
    
    print(f"\n📝 Instruction Counts:")
    for instr, count in sorted(stats['instruction_counts'].items(), key=lambda x: -x[1]):
        marker = "🔥" if instr == 'xmm' else "  "
        print(f"  {marker} {instr:12s}: {count}")
    
    if stats['simd_instructions'] > 0:
        print(f"\n✅ SIMD Instructions Detected: {stats['simd_instructions']}")
    else:
        print(f"\nℹ️  No SIMD instructions (expected for non-vectorized code)")

def compare_with_gcc(source_file):
    """Compare ZeroCC assembly with GCC."""
    print(f"\n{'='*70}")
    print("Comparing with GCC (-O2)")
    print(f"{'='*70}")
    
    # Get GCC assembly
    gcc_cmd = ['gcc', '-O2', '-S', '-fverbose-asm', source_file, '-o', '-']
    try:
        gcc_result = subprocess.run(gcc_cmd, capture_output=True, text=True, timeout=30)
        if gcc_result.returncode != 0:
            print("❌ GCC compilation failed")
            return
    except Exception as e:
        print(f"❌ GCC error: {e}")
        return
    
    # Get ZeroCC assembly
    zerocc_cmd = ['./build/bin/zerocc', '-S', '-O3', source_file, '-o', '-']
    try:
        zerocc_result = subprocess.run(zerocc_cmd, capture_output=True, text=True, timeout=30)
        if zerocc_result.returncode != 0:
            print("❌ ZeroCC compilation failed")
            print(zerocc_result.stderr)
            return
    except Exception as e:
        print(f"❌ ZeroCC error: {e}")
        return
    
    gcc_asm = gcc_result.stdout
    zerocc_asm = zerocc_result.stdout
    
    print(f"\nGCC assembly:     {len(gcc_asm.split(chr(10)))} lines")
    print(f"ZeroCC assembly:  {len(zerocc_asm.split(chr(10)))} lines")
    
    # Compare function counts
    gcc_funcs = len(re.findall(r'^\s*\.globl\s+\w+', gcc_asm, re.MULTILINE))
    zc_funcs = len(re.findall(r'^\s*\.globl\s+\w+', zerocc_asm, re.MULTILINE))
    print(f"\nGCC functions:     {gcc_funcs}")
    print(f"ZeroCC functions:  {zc_funcs}")

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 tools/asm_emitter_validator.py <source.c> [options]")
        print("\nOptions:")
        print("  --gcc     Compare with GCC assembly")
        print("  -O0       Use -O0 optimization level")
        print("  -O2       Use -O2 optimization level")
        print("  -O3       Use -O3 optimization level (default)")
        print("\nExamples:")
        print("  python3 tools/asm_emitter_validator.py test/jit/simple_test.c")
        print("  python3 tools/asm_emitter_validator.py test/bench/array_test.c --gcc")
        sys.exit(1)
    
    source_file = sys.argv[1]
    compare_gcc = '--gcc' in sys.argv
    
    if not os.path.exists(source_file):
        print(f"Error: {source_file} not found")
        sys.exit(1)
    
    # Determine optimization level
    opt_level = '-O3'
    if '-O0' in sys.argv:
        opt_level = '-O0'
    elif '-O2' in sys.argv:
        opt_level = '-O2'
    
    print(f"Running ZeroCC -S {opt_level} on {source_file}...")
    returncode, stdout, stderr = run_zerocc_asm(source_file, opt_level)
    
    if returncode != 0:
        print(f"❌ ZeroCC failed with return code {returncode}")
        print(stderr)
        sys.exit(1)
    
    # Check if output is text or binary
    issues = check_assembly_format(stdout, source_file)
    
    print("\n🔍 Format Validation:")
    for issue in issues:
        print(f"  {issue}")
    
    if any("BINARY" in issue for issue in issues):
        print("\n❌ Assembly emitter is outputting binary instead of text!")
        sys.exit(1)
    
    # Analyze and print statistics
    stats = analyze_assembly_stats(stdout)
    print_assembly_report(source_file, stdout, stats)
    
    if compare_gcc:
        compare_with_gcc(source_file)
    
    print(f"\n{'='*70}")
    print("✅ Assembly output validation complete")
    print(f"{'='*70}\n")

if __name__ == '__main__':
    main()
