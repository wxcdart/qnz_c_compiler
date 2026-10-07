#!/usr/bin/env python3
"""
IR ISA-Agnostic Analyzer

Scans IR lowering code for x86-64 assumptions that break multi-ISA support.

Usage:
    python3 tools/ir_analyzer.py [--verbose]
"""

import re
import sys
import os
from pathlib import Path
from dataclasses import dataclass
from typing import List, Dict, Optional

@dataclass
class Issue:
    severity: str
    file: str
    line: int
    message: str
    code: str
    suggestion: str

X86_PATTERNS = [
    (r'\brax\b', "Hardcoded RAX register", "Use temp registers instead"),
    (r'\brbx\b', "Hardcoded RBX register", "Use temp registers instead"),
    (r'\brbp\b', "Hardcoded RBP register", "Use backend fp_reg"),
    (r'\brsp\b', "Hardcoded RSP register", "Use backend sp_reg"),
]

HARDCODED_VALUES = [
    (r'arg_count\s*>\s*6', "Hardcoded 6 register args", "Use BE(c)->conv->int_arg_reg_count"),
    (r'>\s*6\s*\)', "Hardcoded 6 in argument handling", "Use backend calling convention table"),
]

X86_OPCODES = [
    'OP_PUSH_RAX', 'OP_POP_RAX', 'OP_PUSH_RBX', 'OP_POP_RBX',
    'OP_LEA_RAX_MEM', 'OP_MOV_RAX_RBX', 'OP_MOV_RAX_MEM', 'OP_MOV_MEM_RAX',
    'OP_CQO', 'OP_IDIV',
    'OP_TEST',
    'OP_SETL', 'OP_SETLE', 'OP_SETG', 'OP_SETGE', 'OP_SET_E', 'OP_SET_NE',
]

def analyze_file(filepath: Path, root: Path, verbose: bool = False) -> List[Issue]:
    issues = []
    try:
        with open(filepath, 'r') as f:
            lines = f.readlines()
    except Exception as e:
        return [Issue("error", str(filepath), 0, f"Cannot read file: {e}", "", "")]
    
    rel_path = filepath.relative_to(root) if filepath.is_absolute() else filepath
    
    # Skip backend files - they are SUPPOSED to be ISA-specific
    if 'isa/' in str(rel_path):
        return []
    
    for line_num, line in enumerate(lines, 1):
        for pattern, msg, suggestion in X86_PATTERNS:
            if re.search(pattern, line, re.IGNORECASE):
                if '#' in line and line.index('#') < line.lower().find(pattern.lower()):
                    continue
                issues.append(Issue("warning", str(rel_path), line_num, msg, line.strip(), suggestion))
        
        for pattern, msg, suggestion in HARDCODED_VALUES:
            if re.search(pattern, line):
                issues.append(Issue("error" if "arg_count" in pattern else "warning", 
                                   str(rel_path), line_num, msg, line.strip(), suggestion))
        
        for opcode in X86_OPCODES:
            if opcode in line:
                issues.append(Issue("error", str(rel_path), line_num, 
                                   f"x86-specific opcode: {opcode}", line.strip(),
                                   f"Replace with ISA-neutral opcode"))
    
    return issues

def main():
    import argparse
    parser = argparse.ArgumentParser(description="Analyze IR code for x86-64 assumptions")
    parser.add_argument("--verbose", "-v", action="store_true")
    parser.add_argument("--files", nargs="*")
    args = parser.parse_args()
    
    root = Path.cwd()
    
    if args.files:
        files = [root / f for f in args.files]
    else:
        files = [
            root / "src/zero_cc/parser/ir.h",
            root / "src/zero_cc/parser/ast_lowering_mc.c",
            root / "src/zero_cc/parser/ast_lowering_expr.c",
            root / "src/zero_cc/isa/x86_64.c",
        ]
    
    all_issues = []
    for filepath in files:
        if not filepath.exists():
            continue
        issues = analyze_file(filepath, root, args.verbose)
        all_issues.extend(issues)
    
    errors = [i for i in all_issues if i.severity == "error"]
    warnings = [i for i in all_issues if i.severity == "warning"]
    
    print(f"\n{'='*70}")
    print(f"IR ISA-Agnostic Analysis Report")
    print(f"{'='*70}")
    print(f"Files analyzed: {len(files)}")
    print(f"Errors: {len(errors)}")
    print(f"Warnings: {len(warnings)}")
    print(f"{'='*70}\n")
    
    if errors:
        print("ERRORS (must fix):")
        for issue in errors[:20]:
            print(f"  {issue.file}:{issue.line} - {issue.message}")
            print(f"    {issue.code[:80]}")
            print(f"    Suggestion: {issue.suggestion}")
            print()
    
    if warnings and args.verbose:
        print("WARNINGS:")
        for issue in warnings[:10]:
            print(f"  {issue.file}:{issue.line} - {issue.message}")
            print()
    
    if not all_issues:
        print("✓ No x86-64 assumptions detected!")
    
    return len(errors) > 0

if __name__ == "__main__":
    sys.exit(1 if main() else 0)
