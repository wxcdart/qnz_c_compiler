#!/usr/bin/env python3
"""
Frame Layout Analyzer

Analyzes frame layout calculation in the compiler to detect:
- Alignment issues (16-byte requirement)
- Frame size calculation errors
- Inconsistencies between JIT and assembly paths
- Magic number usage

Usage:
    python3 frame_layout_analyzer.py src/zero_cc/parser/ast_lowering.c
    python3 frame_layout_analyzer.py --check-all
    python3 frame_layout_analyzer.py --generate-report
"""

import argparse
import re
import sys
from pathlib import Path
from dataclasses import dataclass
from typing import List, Dict, Optional


@dataclass
class FrameSizeCalculation:
    """Represents a frame size calculation."""
    file: str
    line: int
    code: str
    alignment: str  # "8-byte", "16-byte", "unknown"
    uses_magic_number: bool
    magic_value: Optional[int]


@dataclass
class PatchOperation:
    """Represents a frame size patch operation."""
    file: str
    line: int
    code: str
    target: str  # "OP_PROLOGUE", "other"


@dataclass
class AnalysisIssue:
    """Represents an issue found in frame layout analysis."""
    severity: str  # "ERROR", "WARNING", "INFO"
    file: str
    line: int
    message: str
    suggestion: str


def analyze_frame_size_calculations(file_path: str) -> List[AnalysisIssue]:
    """Analyze frame size calculations in a file."""
    issues = []
    
    with open(file_path, 'r') as f:
        lines = f.readlines()
    
    # Patterns for frame size calculations
    patterns = {
        '16_byte_align': r'\(\s*\(\s*(\w+)\s*\+\s*15\s*\)\s*&\s*~15\s*\)',
        '8_byte_align': r'\(\s*\(\s*(\w+)\s*\+\s*7\s*\)\s*&\s*~7\s*\)',
        'add_15_align': r'\(\s*\w+\s*\+\s*15\s*\)\s*&\s*~15',
        'add_7_align': r'\(\s*\w+\s*\+\s*7\s*\)\s*&\s*~7',
        'conditional_align': r'if\s*\(\s*\w+\s*==\s*\w+\s*\)\s*\{[^}]*\(\s*\(\s*\w+\s*\+\s*7\s*\)\s*&\s*~7',
    }
    
    in_function = None
    brace_count = 0
    
    for line_num, line in enumerate(lines, 1):
        stripped = line.strip()
        
        # Track function context
        if 'void ' in stripped or 'int ' in stripped or 'size_t ' in stripped:
            func_match = re.search(r'(?:void|int|size_t|char)\s+(\w+)\s*\(', stripped)
            if func_match:
                in_function = func_match.group(1)
        
        # Check for frame size calculations
        if 'current_stack_frame' in line or 'frame_size' in line or 'total_frame_size' in line:
            calc_match = re.search(patterns['16_byte_align'], line)
            if calc_match:
                issues.append(AnalysisIssue(
                    severity="INFO",
                    file=file_path,
                    line=line_num,
                    message=f"Found 16-byte alignment calculation in {in_function or 'unknown'}",
                    suggestion="Verify this is the only frame size calculation"
                ))
            
            calc_match = re.search(patterns['8_byte_align'], line)
            if calc_match:
                issues.append(AnalysisIssue(
                    severity="WARNING",
                    file=file_path,
                    line=line_num,
                    message=f"Found 8-byte alignment calculation in {in_function or 'unknown'}",
                    suggestion="Frame size should be 16-byte aligned for System V ABI"
                ))
            
            # Check for conditional alignment
            if 'if' in line and ('emit_asm' in line or 'emit_asm' in ''.join(lines[max(0, line_num-5):line_num])):
                issues.append(AnalysisIssue(
                    severity="ERROR",
                    file=file_path,
                    line=line_num,
                    message="Conditional frame size calculation detected (JIT vs Assembly)",
                    suggestion="Frame size calculation should be consistent across all paths"
                ))
    
    return issues


def analyze_patch_operations(file_path: str) -> List[AnalysisIssue]:
    """Analyze patch operations (frame size overwrites)."""
    issues = []
    
    with open(file_path, 'r') as f:
        lines = f.readlines()
    
    for line_num, line in enumerate(lines, 1):
        # Check for IR patching
        if 'imm64' in line and '=' in line:
            if 'prologue' in line.lower() or 'OP_PROLOGUE' in line:
                issues.append(AnalysisIssue(
                    severity="WARNING",
                    file=file_path,
                    line=line_num,
                    message="Frame size patch operation detected",
                    suggestion="Consider emitting prologue separately instead of patching"
                ))
        
        # Check for assumptions about prologue position
        if 'ir_get(0)' in line or 'instrs[0]' in line:
            issues.append(AnalysisIssue(
                severity="WARNING",
                file=file_path,
                line=line_num,
                message="Assuming first instruction is prologue",
                suggestion="Add assertion to verify this assumption"
            ))
    
    return issues


def analyze_magic_numbers(file_path: str) -> List[AnalysisIssue]:
    """Analyze magic numbers in frame layout code."""
    issues = []
    
    with open(file_path, 'r') as f:
        lines = f.readlines()
    
    magic_numbers = {
        16: "Alignment constant (16-byte)",
        8: "Alignment constant (8-byte) or register size",
        15: "Alignment mask (0xF)",
        7: "Alignment mask (0x7)",
        64: "Callee-save reserve or stack size",
        40: "Callee-save area (5 regs × 8 bytes)",
    }
    
    for line_num, line in enumerate(lines, 1):
        # Skip comments
        if line.strip().startswith('//'):
            continue
        
        for magic, description in magic_numbers.items():
            # Check for magic number usage
            if re.search(rf'\b{magic}\b', line):
                if '16' in line and 'byte' not in line.lower() and 'align' not in line.lower():
                    issues.append(AnalysisIssue(
                        severity="INFO",
                        file=file_path,
                        line=line_num,
                        message=f"Magic number {magic} ({description})",
                        suggestion="Consider using named constant with documentation"
                    ))
    
    return issues


def analyze_alignment_invariants(file_path: str) -> List[AnalysisIssue]:
    """Analyze alignment invariants and assertions."""
    issues = []
    
    with open(file_path, 'r') as f:
        content = f.read()
    
    # Check for alignment assertions
    if '% 16' not in content and '%16' not in content:
        issues.append(AnalysisIssue(
            severity="WARNING",
            file=file_path,
            line=0,
            message="No 16-byte alignment checks found",
            suggestion="Add assertions to verify frame size is 16-byte aligned"
        ))
    
    if 'assert' not in content:
        issues.append(AnalysisIssue(
            severity="INFO",
            file=file_path,
            line=0,
            message="No assertions found in file",
            suggestion="Consider adding assertions for frame size validation"
        ))
    
    return issues


def analyze_all_files() -> List[AnalysisIssue]:
    """Analyze all relevant source files."""
    issues = []
    
    files_to_check = [
        "src/zero_cc/parser/ast_lowering.c",
        "src/zero_cc/parser/ast_lowering_mc.c",
        "src/zero_cc/parser/asm_emitter.c",
        "src/zero_cc/isa/x86_64.c",
    ]
    
    for file_path in files_to_check:
        if not Path(file_path).exists():
            continue
        
        print(f"Analyzing {file_path}...")
        
        issues.extend(analyze_frame_size_calculations(file_path))
        issues.extend(analyze_patch_operations(file_path))
        issues.extend(analyze_magic_numbers(file_path))
        issues.extend(analyze_alignment_invariants(file_path))
    
    return issues


def generate_report(issues: List[AnalysisIssue]) -> str:
    """Generate a report of all issues."""
    errors = [i for i in issues if i.severity == "ERROR"]
    warnings = [i for i in issues if i.severity == "WARNING"]
    info = [i for i in issues if i.severity == "INFO"]
    
    report = []
    report.append("=" * 70)
    report.append("FRAME LAYOUT ANALYSIS REPORT")
    report.append("=" * 70)
    report.append("")
    report.append(f"Total issues found: {len(issues)}")
    report.append(f"  Errors:   {len(errors)}")
    report.append(f"  Warnings: {len(warnings)}")
    report.append(f"  Info:     {len(info)}")
    report.append("")
    
    if errors:
        report.append("ERRORS:")
        report.append("-" * 70)
        for issue in errors:
            report.append(f"  [{issue.file}:{issue.line}] {issue.message}")
            report.append(f"    Suggestion: {issue.suggestion}")
            report.append("")
    
    if warnings:
        report.append("WARNINGS:")
        report.append("-" * 70)
        for issue in warnings:
            report.append(f"  [{issue.file}:{issue.line}] {issue.message}")
            report.append(f"    Suggestion: {issue.suggestion}")
            report.append("")
    
    if info:
        report.append("INFO:")
        report.append("-" * 70)
        for issue in info:
            report.append(f"  [{issue.file}:{issue.line}] {issue.message}")
            report.append(f"    Suggestion: {issue.suggestion}")
            report.append("")
    
    report.append("=" * 70)
    
    return "\n".join(report)


def main():
    parser = argparse.ArgumentParser(
        description="Analyze frame layout calculations for issues"
    )
    parser.add_argument("file", nargs="?", help="Source file to analyze")
    parser.add_argument("--check-all", action="store_true", help="Check all source files")
    parser.add_argument("--generate-report", action="store_true", help="Generate full report")
    parser.add_argument("--verbose", "-v", action="store_true", help="Show all findings")
    
    args = parser.parse_args()
    
    if args.file:
        issues = []
        issues.extend(analyze_frame_size_calculations(args.file))
        issues.extend(analyze_patch_operations(args.file))
        issues.extend(analyze_magic_numbers(args.file))
        issues.extend(analyze_alignment_invariants(args.file))
        
        if args.generate_report:
            print(generate_report(issues))
        else:
            for issue in issues:
                if args.verbose or issue.severity != "INFO":
                    print(f"[{issue.severity}] {issue.file}:{issue.line}")
                    print(f"  {issue.message}")
                    print(f"  Suggestion: {issue.suggestion}")
                    print()
        
        return 0 if not any(i.severity == "ERROR" for i in issues) else 1
    
    if args.check_all or args.generate_report:
        issues = analyze_all_files()
        
        if args.generate_report:
            print(generate_report(issues))
        else:
            for issue in issues:
                if args.verbose or issue.severity != "INFO":
                    print(f"[{issue.severity}] {issue.file}:{issue.line}")
                    print(f"  {issue.message}")
                    print(f"  Suggestion: {issue.suggestion}")
                    print()
        
        return 0 if not any(i.severity == "ERROR" for i in issues) else 1
    
    parser.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
