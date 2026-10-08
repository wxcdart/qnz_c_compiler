#!/usr/bin/env python3
"""
Assembly Instruction Checker
Validates that emitted assembly uses correct x86-64 instructions.
Helps verify SSE2/SIMD instruction emission.
"""

import os
import sys
import subprocess
import re
from pathlib import Path

# SSE2 instruction patterns
SSE2_INSTRUCTIONS = {
    'arithmetic': {
        'addss': r'\taddss\s',
        'subss': r'\tsubss\s',
        'mulss': r'\tmulss\s',
        'divss': r'\tdivss\s',
        'addps': r'\taddps\s',
        'subps': r'\tsubps\s',
        'mulps': r'\tmulps\s',
        'divps': r'\tdivps\s',
        'addpd': r'\taddpd\s',
        'subpd': r'\tsubpd\s',
        'mulpd': r'\tmulpd\s',
        'divpd': r'\tdivpd\s',
    },
    'move': {
        'movss': r'\tmovss\s',
        'movsd': r'\tmovsd\s',
        'movaps': r'\tmovaps\s',
        'movapd': r'\tmovapd\s',
        'movups': r'\tmovups\s',
        'movupd': r'\tmovupd\s',
        'movdqa': r'\tmovdqa\s',
        'movdqu': r'\tmovdqu\s',
    },
    'conversion': {
        'cvtsi2ss': r'\tcvtsi2ss\s',
        'cvtsi2sd': r'\tcvtsi2sd\s',
        'cvtss2si': r'\tcvtss2si\s',
        'cvtsd2si': r'\tcvtsd2si\s',
        'cvttss2si': r'\tcvttss2si\s',
        'cvttsd2si': r'\tcvttsd2si\s',
        'cvtss2sd': r'\tcvtss2sd\s',
        'cvtsd2ss': r'\tcvtsd2ss\s',
    },
    'comparison': {
        'cmpss': r'\tcmpss\s',
        'cmpps': r'\tcmpps\s',
        'cmpsd': r'\tcmpsd\s',
        'cmppd': r'\tcmppd\s',
        'ucomiss': r'\tucomiss\s',
        'ucomisd': r'\tucomisd\s',
        'comiss': r'\tcomiss\s',
        'comisd': r'\tcomisd\s',
    },
    'logical': {
        'andps': r'\tandps\s',
        'andpd': r'\tandpd\s',
        'orps': r'\torps\s',
        'orpd': r'\torpd\s',
        'xorps': r'\txorps\s',
        'xorpd': r'\txorpd\s',
    },
    'shuffle': {
        'shufps': r'\tshufps\s',
        'shufpd': r'\tshufpd\s',
        'unpcklps': r'\tunpcklps\s',
        'unpckhps': r'\tunpckhps\s',
        'unpcklpd': r'\tunpcklpd\s',
        'unpckhpds': r'\tunpckhpds\s',
    },
    'horizontal': {
        'haddps': r'\thaddps\s',
        'haddpd': r'\thaddpd\s',
        'hsubps': r'\thsubps\s',
        'hsubpd': r'\thsubpd\s',
    },
    'misc': {
        'sqrts': r'\tsqrt[sd]\s',
        'rcpss': r'\trcpss\s',
        'rsqrtss': r'\trsqrtss\s',
        'pause': r'\tpause\s',
        'mfence': r'\tmfence\s',
        'lfence': r'\tlfence\s',
        'sfence': r'\tsfence\s',
    },
}

def extract_assembly(source_file, zerocc_path='./build/bin/zerocc', opt_level='-O3'):
    """Extract assembly from ZeroCC."""
    cmd = [zerocc_path, '-S', opt_level, source_file, '-o', '-']
    
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        if result.returncode != 0:
            print(f"Error: ZeroCC failed: {result.stderr}")
            return None
        return result.stdout
    except Exception as e:
        print(f"Error: {e}")
        return None

def check_instructions(asm_content):
    """Check for SSE2 instructions in assembly."""
    found = {category: {} for category in SSE2_INSTRUCTIONS}
    not_found = {category: [] for category in SSE2_INSTRUCTIONS}
    
    for category, instructions in SSE2_INSTRUCTIONS.items():
        for instr_name, pattern in instructions.items():
            if re.search(pattern, asm_content, re.IGNORECASE):
                found[category][instr_name] = True
            else:
                not_found[category].append(instr_name)
    
    return found, not_found

def print_instruction_report(found, not_found):
    """Print instruction coverage report."""
    print(f"\n{'='*70}")
    print("SSE2 Instruction Coverage Report")
    print(f"{'='*70}")
    
    total_found = 0
    total_missing = 0
    
    for category in SSE2_INSTRUCTIONS:
        found_count = len(found[category])
        missing_count = len(not_found[category])
        total_found += found_count
        total_missing += missing_count
        
        print(f"\n{category.upper()}:")
        print(f"  Found: {found_count}, Missing: {missing_count}")
        
        if found[category]:
            print(f"  ✅ Implemented: {', '.join(found[category].keys())}")
        
        if not_found[category]:
            print(f"  ❌ Not found: {', '.join(not_found[category][:5])}")
            if len(not_found[category]) > 5:
                print(f"       ... and {len(not_found[category]) - 5} more")
    
    print(f"\n{'='*70}")
    print(f"Total: {total_found} found, {total_missing} missing")
    print(f"Coverage: {total_found / (total_found + total_missing) * 100:.1f}%")
    print(f"{'='*70}")

def check_xmm_register_usage(asm_content):
    """Check XMM register usage patterns."""
    print(f"\n{'='*70}")
    print("XMM Register Usage Analysis")
    print(f"{'='*70}")
    
    # Count XMM register usage
    xmm_pattern = r'%xmm(\d+)'
    xmm_matches = re.findall(xmm_pattern, asm_content)
    
    if not xmm_matches:
        print("  ℹ️  No XMM registers used (no SIMD)")
        return
    
    # Count per register
    xmm_counts = {}
    for reg in xmm_matches:
        xmm_counts[reg] = xmm_counts.get(reg, 0) + 1
    
    print(f"\n  XMM registers used: {len(xmm_counts)}")
    for reg, count in sorted(xmm_counts.items(), key=lambda x: -x[1]):
        print(f"    %xmm{reg}: {count} uses")
    
    # Check for potential issues
    print(f"\n  Potential issues:")
    
    # Check for uninitialized XMM usage
    defined = set()
    used_before_def = []
    
    for line in asm_content.split('\n'):
        # Check for definitions (movss, movaps, addss, etc.)
        if re.search(r'\t(mov|add|sub|mul|div|xor|or|and)[spd]+\s+%xmm(\d+)', line):
            match = re.search(r'%xmm(\d+)', line)
            if match:
                defined.add(match.group(1))
        
        # Check for uses
        uses = re.findall(r'%xmm(\d+)', line)
        for use in uses:
            if use not in defined and not line.strip().startswith('#'):
                if use not in used_before_def:
                    used_before_def.append(use)
    
    if used_before_def:
        print(f"  ⚠️  XMM registers used before definition: {', '.join(used_before_def)}")
    else:
        print(f"  ✅ No obvious uninitialized XMM usage")

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 tools/asm_instruction_checker.py <source.c> [options]")
        print("\nOptions:")
        print("  -O0, -O2, -O3  Optimization level (default: -O3)")
        print("  --full         Show all missing instructions")
        print("\nExamples:")
        print("  python3 tools/asm_instruction_checker.py test/bench/array_sum.c")
        print("  python3 tools/asm_instruction_checker.py test/bench/vector_add.c --full")
        sys.exit(1)
    
    source_file = sys.argv[1]
    show_all = '--full' in sys.argv
    
    if not os.path.exists(source_file):
        print(f"Error: {source_file} not found")
        sys.exit(1)
    
    print(f"Analyzing assembly for {source_file}...")
    
    asm_content = extract_assembly(source_file)
    if not asm_content:
        sys.exit(1)
    
    found, not_found = check_instructions(asm_content)
    print_instruction_report(found, not_found)
    
    if show_all:
        print(f"\n{'='*70}")
        print("All Missing Instructions:")
        print(f"{'='*70}")
        for category in SSE2_INSTRUCTIONS:
            if not_found[category]:
                print(f"\n{category.upper()}:")
                for instr in not_found[category]:
                    print(f"  - {instr}")
    
    check_xmm_register_usage(asm_content)
    
    print(f"\n{'='*70}")
    print("✅ Instruction check complete")
    print(f"{'='*70}\n")

if __name__ == '__main__':
    main()
