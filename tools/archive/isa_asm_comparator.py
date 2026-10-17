#!/usr/bin/env python3
"""
ISA Assembly Comparator
Compares assembly output across multiple ISAs to detect ISA-specific bugs.

This tool compiles the same C source for multiple ISAs and compares:
- Instruction patterns and counts
- Prologue/epilogue structure
- Register usage
- Stack frame layout

Usage:
    python3 tools/isa_asm_comparator.py test.c --isas x86_64 aarch64 riscv64
"""

import argparse
import subprocess
import sys
import re
from pathlib import Path
from dataclasses import dataclass
from typing import Dict, List, Optional


@dataclass
class ISAStats:
    """Statistics for a single ISA."""
    isa: str
    success: bool
    asm_content: str = ""
    error: str = ""
    
    # Instruction counts
    instruction_counts: Dict[str, int] = None
    function_count: int = 0
    prologue_ok: bool = False
    epilogue_ok: bool = False
    
    # Register usage
    reg_usage: Dict[str, int] = None
    
    def __post_init__(self):
        if self.instruction_counts is None:
            self.instruction_counts = {}
        if self.reg_usage is None:
            self.reg_usage = {}


# ISA-specific instruction patterns
ISA_INSTRUCTIONS = {
    'x86_64': {
        'prologue': [r'\tpush\s+%rbp', r'\tmovq\s+%rsp,%rbp', r'\tsubq\s+\$'],
        'epilogue': [r'\tleave', r'\tret'],
        'instructions': [
            ('mov', r'\t(movq|movl|movw|movb)\s+'),
            ('add', r'\taddq\s+'),
            ('sub', r'\tsubq\s+'),
            ('call', r'\tcall\s+'),
            ('push', r'\tpushq\s+'),
            ('pop', r'\tpopq\s+'),
        ],
        'registers': [r'%rax', r'%rbx', r'%rcx', r'%rdx', r'%rsi', r'%rdi', r'%r[0-9]{1,2}'],
    },
    'aarch64': {
        'prologue': [r'\stp[dx]\d+,.*\[sp', r'\tadd\s+sp,sp,-?\d+'],
        'epilogue': [r'\tldp\s+x[0-9]+', r'\tret'],
        'instructions': [
            ('mov', r'\t(mov|orr)\s+'),
            ('add', r'\tadd\s+'),
            ('sub', r'\tsub\s+'),
            ('call', r'\tbl\s+'),
            ('str', r'\tstr\s+'),
            ('ldr', r'\tldr\s+'),
        ],
        'registers': [r'\bx[0-9]{1,2}', r'\bw[0-9]{1,2}'],
    },
    'riscv64': {
        'prologue': [r'\taddi\s+sp,sp,-?\d+', r'\tsd\s+s[0-9]+'],
        'epilogue': [r'\tld\s+s[0-9]+', r'\tret'],
        'instructions': [
            ('addi', r'\taddi\s+'),
            ('add', r'\tadd\s+'),
            ('sub', r'\tsub\s+'),
            ('call', r'\tjal\s+'),
            ('sd', r'\tsd\s+'),
            ('ld', r'\tld\s+'),
        ],
        'registers': [r'\ba[0-7]', r'\bs[0-9]{0,2}', r'\bt[0-6]'],
    },
}


def run_zerocc_asm(source_file: Path, target: str, opt_level: str = '-O3') -> tuple:
    """Run ZeroCC with -S flag to generate assembly."""
    cmd = ['./build/bin/zerocc', '-S', opt_level, '--target', target, str(source_file), '-o', '-']
    
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        return result.returncode, result.stdout, result.stderr
    except Exception as e:
        return -1, "", str(e)


def count_instructions(asm_content: str, isa: str) -> Dict[str, int]:
    """Count instructions for a specific ISA."""
    counts = {}
    
    if isa not in ISA_INSTRUCTIONS:
        return counts
    
    for name, pattern in ISA_INSTRUCTIONS[isa]['instructions']:
        matches = re.findall(pattern, asm_content)
        if matches:
            counts[name] = len(matches)
    
    return counts


def count_registers(asm_content: str, isa: str) -> Dict[str, int]:
    """Count register usage for a specific ISA."""
    counts = {}
    
    if isa not in ISA_INSTRUCTIONS:
        return counts
    
    for reg_pattern in ISA_INSTRUCTIONS[isa]['registers']:
        matches = re.findall(reg_pattern, asm_content)
        for reg in matches:
            # Normalize register names
            reg_name = re.sub(r'[0-9]+', 'N', reg)  # Group x0-x31 as xN
            counts[reg_name] = counts.get(reg_name, 0) + 1
    
    return counts


def check_prologue(asm_content: str, isa: str) -> bool:
    """Check if prologue pattern is present."""
    if isa not in ISA_INSTRUCTIONS:
        return False
    
    for pattern in ISA_INSTRUCTIONS[isa]['prologue']:
        if re.search(pattern, asm_content):
            return True
    return False


def check_epilogue(asm_content: str, isa: str) -> bool:
    """Check if epilogue pattern is present."""
    if isa not in ISA_INSTRUCTIONS:
        return False
    
    for pattern in ISA_INSTRUCTIONS[isa]['epilogue']:
        if re.search(pattern, asm_content):
            return True
    return False


def count_functions(asm_content: str) -> int:
    """Count function definitions."""
    return len(re.findall(r'^\s*\.\s*globl\s+\w+', asm_content, re.MULTILINE))


def analyze_isa(source_file: Path, isa: str, opt_level: str) -> ISAStats:
    """Analyze assembly for a specific ISA."""
    returncode, stdout, stderr = run_zerocc_asm(source_file, isa, opt_level)
    
    if returncode != 0:
        return ISAStats(
            isa=isa,
            success=False,
            error=stderr
        )
    
    # Analyze content
    stats = ISAStats(
        isa=isa,
        success=True,
        asm_content=stdout,
        instruction_counts=count_instructions(stdout, isa),
        function_count=count_functions(stdout),
        prologue_ok=check_prologue(stdout, isa),
        epilogue_ok=check_epilogue(stdout, isa),
        reg_usage=count_registers(stdout, isa),
    )
    
    return stats


def compare_stats(results: List[ISAStats]) -> Dict:
    """Compare statistics across ISAs."""
    comparison = {
        'function_count_diff': {},
        'instruction_diff': {},
        'prologue_issues': [],
        'epilogue_issues': [],
    }
    
    # Get baseline (first successful)
    baseline = None
    for r in results:
        if r.success:
            baseline = r
            break
    
    if not baseline:
        return comparison
    
    # Compare function counts
    for r in results:
        if r.success and r.isa != baseline.isa:
            diff = r.function_count - baseline.function_count
            comparison['function_count_diff'][r.isa] = diff
    
    # Compare instruction counts
    all_instrs = set()
    for r in results:
        if r.success:
            all_instrs.update(r.instruction_counts.keys())
    
    for instr in all_instrs:
        counts = {}
        for r in results:
            if r.success:
                counts[r.isa] = r.instruction_counts.get(instr, 0)
        
        # Find max and min
        if counts:
            max_isa = max(counts, key=counts.get)
            min_isa = min(counts, key=counts.get)
            if counts[max_isa] != counts[min_isa]:
                comparison['instruction_diff'][instr] = counts
    
    # Check prologue/epilogue
    for r in results:
        if r.success:
            if not r.prologue_ok:
                comparison['prologue_issues'].append(r.isa)
            if not r.epilogue_ok:
                comparison['epilogue_issues'].append(r.isa)
    
    return comparison


def print_report(results: List[ISAStats], comparison: Dict):
    """Print comparison report."""
    print(f"\n{'='*70}")
    print(f"ISA Assembly Comparison Report")
    print(f"{'='*70}\n")
    
    # Summary table
    print(f"{'ISA':<12} {'Success':<10} {'Functions':<12} {'Prologue':<12} {'Epilogue':<12}")
    print("-" * 70)
    
    for r in results:
        if r.success:
            status = "✓ PASS"
            prologue = "✓" if r.prologue_ok else "✗"
            epilogue = "✓" if r.epilogue_ok else "✗"
            print(f"{r.isa:<12} {status:<10} {r.function_count:<12} {prologue:<12} {epilogue:<12}")
        else:
            print(f"{r.isa:<12} ✗ FAIL    {r.error[:40]}")
    
    print()
    
    # Instruction comparison
    if comparison['instruction_diff']:
        print("Instruction Count Differences:")
        for instr, counts in comparison['instruction_diff'].items():
            print(f"  {instr}: {counts}")
        print()
    
    # Prologue/Epilogue issues
    if comparison['prologue_issues']:
        print(f"⚠️  Prologue issues: {', '.join(comparison['prologue_issues'])}")
    if comparison['epilogue_issues']:
        print(f"⚠️  Epilogue issues: {', '.join(comparison['epilogue_issues'])}")
    
    if not comparison['prologue_issues'] and not comparison['epilogue_issues']:
        print("✅ All ISAs have valid prologue/epilogue patterns")
    
    print()


def main():
    parser = argparse.ArgumentParser(description='Compare assembly across ISAs')
    parser.add_argument('source', help='C source file to compile')
    parser.add_argument('--isas', nargs='+', default=['x86_64', 'aarch64'],
                        choices=['x86_64', 'aarch64', 'riscv64'],
                        help='ISAs to compare')
    parser.add_argument('-O', '--opt-level', default='-O3', help='Optimization level')
    parser.add_argument('-v', '--verbose', action='store_true', help='Verbose output')
    args = parser.parse_args()
    
    source_file = Path(args.source)
    if not source_file.exists():
        print(f"Error: {source_file} not found", file=sys.stderr)
        sys.exit(1)
    
    # Analyze each ISA
    print(f"Analyzing assembly for {', '.join(args.isas)}...")
    results = []
    for isa in args.isas:
        print(f"  {isa}...", end=" ", flush=True)
        stats = analyze_isa(source_file, isa, args.opt_level)
        results.append(stats)
        if stats.success:
            print(f"✓ ({stats.function_count} functions)")
        else:
            print(f"✗ {stats.error[:30]}")
    
    # Compare
    comparison = compare_stats(results)
    
    # Print report
    print_report(results, comparison)
    
    # Exit status
    all_success = all(r.success for r in results)
    sys.exit(0 if all_success else 1)


if __name__ == "__main__":
    main()
