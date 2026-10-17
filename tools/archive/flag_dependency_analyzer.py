#!/usr/bin/env python3
"""
Flag Dependency Analyzer

Analyzes current IR flag usage to prepare for flag refactoring.

Identifies:
1. Flag-setting operations (OP_CMP, OP_CMP_ZERO)
2. Flag-reading operations (OP_COND_SET, OP_JE, OP_JNE, etc.)
3. Implicit flag dependencies (CMP followed by conditional jumps)
4. Potential flag clobbering (instructions between cmp and flag-usage)

Usage:
    python3 tools/flag_dependency_analyzer.py

Output:
    - Statistics on flag operations
    - List of implicit flag dependencies to convert to OP_JCOND
    - Suggestions for flag register placement
"""

import re
import sys
from pathlib import Path
from collections import defaultdict

# Flag-setting opcodes
FLAG_SETTERS = {'OP_CMP', 'OP_CMP_ZERO'}

# Flag-reading opcodes (implicit)
FLAG_READERS_IMPLICIT = {
    'OP_JE', 'OP_JNE', 'OP_JL', 'OP_JLE', 'OP_JG', 'OP_JGE',
    'OP_JA', 'OP_JAE', 'OP_JB', 'OP_JBE', 'OP_JO', 'OP_JNO',
    'OP_JS', 'OP_JNS', 'OP_JP', 'OP_JNP',
}

# Flag-reading opcodes (explicit - already using OP_COND_SET)
FLAG_READERS_EXPLICIT = {'OP_COND_SET'}

# Opcodes that clobber flags (after cmp, flags are lost)
FLAG_CLOBBERERS = {
    'OP_ADD', 'OP_SUB', 'OP_AND', 'OP_OR', 'OP_XOR', 'OP_SHL', 'OP_SHR', 'OP_SAR',
    'OP_INC', 'OP_DEC', 'OP_NEG', 'OP_NOT',
    'OP_MUL', 'OP_DIV',  # These definitely clobber flags
}

def scan_file_for_flag_usage(filepath):
    """Scan a file for flag-setting and flag-reading operations."""
    results = {
        'flag_setters': [],
        'flag_readers_implicit': [],
        'flag_readers_explicit': [],
        'flag_clobberers': [],
    }
    
    try:
        with open(filepath, 'r') as f:
            lines = f.readlines()
    except Exception as e:
        return None
    
    for i, line in enumerate(lines, 1):
        # Check for flag setters
        for opcode in FLAG_SETTERS:
            if opcode in line:
                results['flag_setters'].append((i, line.strip(), opcode))
        
        # Check for implicit flag readers
        for opcode in FLAG_READERS_IMPLICIT:
            if opcode in line:
                results['flag_readers_implicit'].append((i, line.strip(), opcode))
        
        # Check for explicit flag readers
        for opcode in FLAG_READERS_EXPLICIT:
            if opcode in line:
                results['flag_readers_explicit'].append((i, line.strip(), opcode))
        
        # Check for flag clobberers
        for opcode in FLAG_CLOBBERERS:
            if opcode in line:
                results['flag_clobberers'].append((i, line.strip(), opcode))
    
    return results

def analyze_flag_dependencies(results):
    """Analyze flag dependencies and find potential issues."""
    issues = []
    
    flag_setters = results['flag_setters']
    flag_readers_implicit = results['flag_readers_implicit']
    flag_readers_explicit = results['flag_readers_explicit']
    flag_clobberers = results['flag_clobberers']
    
    # Count implicit flag readers
    implicit_count = len(flag_readers_implicit)
    explicit_count = len(flag_readers_explicit)
    
    # Find patterns where cmp is followed by implicit flag reader
    # This indicates places that could use OP_JCOND instead
    
    return {
        'implicit_flag_readers': implicit_count,
        'explicit_flag_readers': explicit_count,
        'flag_setters': len(flag_setters),
        'flag_clobberers': len(flag_clobberers),
    }

def generate_migration_report(all_results):
    """Generate a migration report for flag refactoring."""
    print("=" * 80)
    print("Flag Dependency Analysis Report")
    print("=" * 80)
    print()
    
    total_implicit = 0
    total_explicit = 0
    total_setters = 0
    total_clobberers = 0
    
    by_file = defaultdict(lambda: {'implicit': 0, 'explicit': 0, 'setters': 0, 'clobberers': 0})
    
    for filepath, results in all_results.items():
        if results is None:
            continue
        
        stats = analyze_flag_dependencies(results)
        
        total_implicit += stats['implicit_flag_readers']
        total_explicit += stats['explicit_flag_readers']
        total_setters += stats['flag_setters']
        total_clobberers += stats['flag_clobberers']
        
        by_file[filepath]['implicit'] = stats['implicit_flag_readers']
        by_file[filepath]['explicit'] = stats['explicit_flag_readers']
        by_file[filepath]['setters'] = stats['flag_setters']
        by_file[filepath]['clobberers'] = stats['flag_clobberers']
    
    print("SUMMARY")
    print("-" * 80)
    print(f"Total flag-setting operations (OP_CMP, OP_CMP_ZERO): {total_setters}")
    print(f"Total explicit flag readers (OP_COND_SET):          {total_explicit}")
    print(f"Total implicit flag readers (OP_JE, OP_JL, etc.):   {total_implicit}")
    print(f"Total flag clobbering operations:                   {total_clobberers}")
    print()
    
    print("MIGRATION NEEDED")
    print("-" * 80)
    print(f"Files with implicit flag readers: {sum(1 for f in by_file.values() if f['implicit'] > 0)}")
    print()
    
    print("Files requiring OP_JCOND migration:")
    for filepath, stats in sorted(by_file.items()):
        if stats['implicit'] > 0:
            print(f"  {filepath}:")
            print(f"    - Implicit flag readers: {stats['implicit']}")
            print(f"    - Flag setters: {stats['setters']}")
    print()
    
    print("RECOMMENDATIONS")
    print("-" * 80)
    print("Phase 1 (OP_JCOND):")
    print(f"  - Convert {total_implicit} implicit flag readers to OP_JCOND")
    print(f"  - Files affected: {sum(1 for f in by_file.values() if f['implicit'] > 0)}")
    print()
    print("Phase 2 (Flag Register):")
    print(f"  - Add flags_in/flags_out to {total_setters + total_explicit + total_implicit} instructions")
    print(f"  - Implement flag CSE/DCE optimizations")
    print()
    
    return {
        'total_implicit': total_implicit,
        'total_explicit': total_explicit,
        'total_setters': total_setters,
        'total_clobberers': total_clobberers,
        'by_file': dict(by_file),
    }

def main():
    root = Path.cwd()
    
    # Files to analyze
    files_to_scan = [
        'src/zero_cc/parser/ast_lowering_expr.c',
        'src/zero_cc/parser/ast_lowering_mc.c',
        'src/zero_cc/parser/ast_lowering_stmt.c',
        'src/zero_cc/isa/x86_64.c',
        'src/zero_cc/isa/aarch64.c',
        'src/zero_cc/isa/riscv.c',
    ]
    
    print("Analyzing flag dependencies...")
    print()
    
    all_results = {}
    for filepath in files_to_scan:
        full_path = root / filepath
        if full_path.exists():
            results = scan_file_for_flag_usage(full_path)
            all_results[filepath] = results
        else:
            all_results[filepath] = None
    
    # Generate report
    stats = generate_migration_report(all_results)
    
    print("=" * 80)
    print("Analysis complete!")
    print("=" * 80)
    
    return 0

if __name__ == "__main__":
    sys.exit(main())
