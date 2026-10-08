#!/usr/bin/env python3
"""
Stack Alignment Checker

Verifies that stack alignment is correct according to System V AMD64 ABI:
- RSP must be 16-byte aligned before CALL instruction
- Prologue must maintain alignment
- Frame size must be 16-byte aligned

Usage:
    python3 stack_alignment_checker.py --asm-file test.s
    python3 stack_alignment_checker.py --test test.c
    python3 stack_alignment_checker.py --check-all
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path
from dataclasses import dataclass
from typing import List, Optional, Tuple


@dataclass
class CallSite:
    """Represents a call site with stack alignment information."""
    function: str
    line: int
    instruction: str
    rsp_before: Optional[int]  # Stack pointer value before call (mod 16)
    is_aligned: bool
    issue: Optional[str]


@dataclass
class PrologueAnalysis:
    """Analysis of a function's prologue."""
    function: str
    frame_size: int
    initial_rsp_mod_16: int  # RSP mod 16 after push rbp
    final_rsp_mod_16: int    # RSP mod 16 after sub rsp, frame_size
    is_aligned: bool
    issue: Optional[str]


def calculate_stack_alignment(asm_file: str) -> Tuple[List[PrologueAnalysis], List[CallSite]]:
    """Calculate stack alignment for all functions in assembly file."""
    prologues = []
    call_sites = []
    
    with open(asm_file, 'r') as f:
        lines = f.readlines()
    
    current_function = None
    current_frame_size = 0
    rsp_mod_16 = 0  # Track RSP mod 16
    
    for line_num, line in enumerate(lines, 1):
        stripped = line.strip()
        
        # Skip comments and empty lines
        if not stripped or stripped.startswith('#') or stripped.startswith('//'):
            continue
        
        # Track function entry
        func_match = re.match(r'^(\w+):', stripped)
        if func_match:
            if current_function:
                # Save previous function's prologue analysis
                if current_frame_size > 0:
                    final_mod = (rsp_mod_16 - current_frame_size) % 16
                    is_aligned = (final_mod == 0)
                    issue = None if is_aligned else f"RSP = {final_mod} mod 16 (should be 0)"
                    prologues.append(PrologueAnalysis(
                        function=current_function,
                        frame_size=current_frame_size,
                        initial_rsp_mod_16=rsp_mod_16,
                        final_rsp_mod_16=final_mod,
                        is_aligned=is_aligned,
                        issue=issue
                    ))
            
            current_function = func_match.group(1)
            current_frame_size = 0
            rsp_mod_16 = 8  # After function entry, RSP is 8 mod 16 (call pushed 8 bytes)
            continue
        
        if not current_function:
            continue
        
        # Track prologue instructions
        if re.match(r'push\s+%?rbp', stripped):
            rsp_mod_16 = (rsp_mod_16 - 8) % 16  # push rbp
        
        elif re.match(r'mov\s+%?rsp,%?rbp', stripped):
            pass  # mov doesn't change RSP
        
        elif re.match(r'sub\s+\$?(\d+),', stripped):
            match = re.search(r'\$?(\d+)', stripped)
            if match:
                frame_size = int(match.group(1))
                current_frame_size = frame_size
                rsp_mod_16 = (rsp_mod_16 - frame_size) % 16
        
        elif re.match(r'add\s+\$?(\d+),', stripped):
            match = re.search(r'\$?(\d+)', stripped)
            if match:
                rsp_mod_16 = (rsp_mod_16 + int(match.group(1))) % 16
        
        elif re.match(r'call', stripped):
            # Check alignment before call
            is_aligned = (rsp_mod_16 == 0)
            issue = None if is_aligned else f"RSP = {rsp_mod_16} mod 16 (should be 0 before call)"
            call_sites.append(CallSite(
                function=current_function,
                line=line_num,
                instruction=stripped,
                rsp_before=rsp_mod_16,
                is_aligned=is_aligned,
                issue=issue
            ))
        
        elif re.match(r'ret', stripped):
            # End of function
            if current_frame_size > 0:
                final_mod = rsp_mod_16
                is_aligned = (final_mod == 0)
                issue = None if is_aligned else f"RSP = {final_mod} mod 16 at return"
                prologues.append(PrologueAnalysis(
                    function=current_function,
                    frame_size=current_frame_size,
                    initial_rsp_mod_16=8,  # After push rbp
                    final_rsp_mod_16=final_mod,
                    is_aligned=is_aligned,
                    issue=issue
                ))
            current_function = None
            current_frame_size = 0
            rsp_mod_16 = 0
    
    return prologues, call_sites


def validate_prologues(prologues: List[PrologueAnalysis]) -> List[str]:
    """Validate all prologues."""
    issues = []
    
    for prologue in prologues:
        if not prologue.is_aligned:
            issues.append(
                f"Function '{prologue.function}': "
                f"frame_size={prologue.frame_size}, "
                f"RSP={prologue.final_rsp_mod_16} mod 16 "
                f"(should be 0)"
            )
    
    return issues


def validate_call_sites(call_sites: List[CallSite]) -> List[str]:
    """Validate all call sites."""
    issues = []
    
    for site in call_sites:
        if not site.is_aligned:
            issues.append(
                f"Function '{site.function}' line {site.line}: "
                f"RSP={site.rsp_before} mod 16 before call "
                f"(should be 0)"
            )
    
    return issues


def check_assembly_file(asm_file: str, verbose: bool = False) -> Tuple[int, int]:
    """Check assembly file for stack alignment issues."""
    prologues, call_sites = calculate_stack_alignment(asm_file)
    
    prologue_issues = validate_prologues(prologues)
    call_issues = validate_call_sites(call_sites)
    
    total_prologues = len(prologues)
    total_calls = len(call_sites)
    total_issues = len(prologue_issues) + len(call_issues)
    
    print(f"Stack Alignment Analysis for: {asm_file}")
    print("=" * 60)
    print(f"Functions analyzed: {total_prologues}")
    print(f"Call sites analyzed: {total_calls}")
    print(f"Issues found: {total_issues}")
    print()
    
    if verbose or prologue_issues:
        print("Prologue Issues:")
        print("-" * 60)
        for issue in prologue_issues:
            print(f"  ❌ {issue}")
        if not prologue_issues:
            print("  ✅ All prologues have correct alignment")
        print()
    
    if verbose or call_issues:
        print("Call Site Issues:")
        print("-" * 60)
        for issue in call_issues:
            print(f"  ❌ {issue}")
        if not call_issues:
            print("  ✅ All call sites have correct alignment")
        print()
    
    return total_issues, total_prologues + total_calls


def check_c_file(c_file: str, verbose: bool = False) -> Tuple[int, int]:
    """Compile C file to assembly and check alignment."""
    try:
        import tempfile
        
        # Compile to assembly
        result = subprocess.run(
            ["./build/bin/zerocc", "-S", "-o", "-", c_file],
            capture_output=True,
            text=True,
            check=True
        )
        
        # Write to temp file
        with tempfile.NamedTemporaryFile(mode='w', suffix='.s', delete=False) as f:
            f.write(result.stdout)
            temp_file = f.name
        
        return check_assembly_file(temp_file, verbose)
        
    except subprocess.CalledProcessError as e:
        print(f"Error compiling {c_file}: {e}")
        return 1, 0


def check_all_tests(verbose: bool = False) -> Tuple[int, int]:
    """Check all test files."""
    test_dir = Path("test/jit")
    total_issues = 0
    total_checks = 0
    
    for test_file in test_dir.glob("*.c"):
        print(f"\nChecking {test_file.name}...")
        issues, checks = check_c_file(str(test_file), verbose)
        total_issues += issues
        total_checks += checks
    
    return total_issues, total_checks


def main():
    parser = argparse.ArgumentParser(
        description="Check stack alignment in assembly code"
    )
    parser.add_argument("--asm-file", "-f", help="Assembly file to check")
    parser.add_argument("--test", "-t", help="C source file to compile and check")
    parser.add_argument("--check-all", action="store_true", help="Check all test files")
    parser.add_argument("--verbose", "-v", action="store_true", help="Show all details")
    
    args = parser.parse_args()
    
    if args.asm_file:
        issues, checks = check_assembly_file(args.asm_file, args.verbose)
        return 0 if issues == 0 else 1
    
    if args.test:
        issues, checks = check_c_file(args.test, args.verbose)
        return 0 if issues == 0 else 1
    
    if args.check_all:
        issues, checks = check_all_tests(args.verbose)
        print(f"\n{'=' * 60}")
        print(f"Total: {issues} issues in {checks} checks")
        return 0 if issues == 0 else 1
    
    parser.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
