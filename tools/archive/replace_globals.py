#!/usr/bin/env python3
"""
Automated global variable replacement for Task 61.
Usage: python3 tools/replace_globals.py --dry-run  # First, see what will change
       python3 tools/replace_globals.py --apply    # Apply changes
"""

import os
import re
import sys
import json
import argparse
from pathlib import Path

# Global variables to replace and their new paths
GLOBAL_REPLACEMENTS = {
    'g_ir_emitter_ctx': 'c->lowering',
    'g_ir_epilogue_label': 'c->lowering.epilogue_label',
    'g_loop_label_stack': 'c->lowering.loop_stack',
    'g_loop_label_depth': 'c->lowering.loop_depth',
    'g_user_labels': 'c->lowering.user_labels',
    'g_user_label_count': 'c->lowering.user_label_count',
}

# Files to modify (from dependency graph)
FILES_TO_MODIFY = [
    'src/zero_cc/parser/ast_lowering.h',
    'src/zero_cc/parser/ast_lowering_expr.h',
    'src/zero_cc/parser/ast_lowering_mc.h',
    'src/zero_cc/parser/ast_lowering_stmt.h',
]

def load_deps():
    """Load dependency graph."""
    deps_file = Path(__file__).parent.parent / 'deps_graph.json'
    with open(deps_file) as f:
        return json.load(f)

def find_global_usages(deps, global_name):
    """Find all files using a global."""
    if global_name in deps['globals']:
        return deps['globals'][global_name].get('used_in', [])
    return []

def replace_in_file(filepath, dry_run=True):
    """Replace globals in a file."""
    if not os.path.exists(filepath):
        print(f"  ⚠️  File not found: {filepath}")
        return []
    
    with open(filepath, 'r') as f:
        original = f.read()
    
    changes = []
    modified = original
    
    for old_name, new_name in GLOBAL_REPLACEMENTS.items():
        # Pattern to match the global variable (word boundary)
        pattern = r'\b' + re.escape(old_name) + r'\b'
        matches = list(re.finditer(pattern, modified))
        
        for match in matches:
            start = max(0, match.start() - 50)
            end = min(len(modified), match.end() + 50)
            context = modified[start:end].replace('\n', ' ')
            
            changes.append({
                'old': old_name,
                'new': new_name,
                'line': modified[:match.start()].count('\n') + 1,
                'context': context
            })
        
        modified = re.sub(pattern, new_name, modified)
    
    if not dry_run and modified != original:
        with open(filepath, 'w') as f:
            f.write(modified)
    
    return changes

def main():
    parser = argparse.ArgumentParser(description='Replace global variables for Task 61')
    parser.add_argument('--dry-run', action='store_true', help='Show what would change')
    parser.add_argument('--apply', action='store_true', help='Apply changes')
    parser.add_argument('--file', help='Specific file to modify')
    args = parser.parse_args()
    
    if not args.dry_run and not args.apply:
        args.dry_run = True
    
    deps = load_deps()
    
    print("=" * 70)
    print("Task 61: Global Variable Replacement")
    print("=" * 70)
    
    files = [args.file] if args.file else FILES_TO_MODIFY
    
    total_changes = 0
    for filepath in files:
        print(f"\n{filepath}:")
        changes = replace_in_file(filepath, dry_run=args.dry_run)
        if changes:
            for change in changes:
                print(f"  Line {change['line']}: {change['old']} → {change['new']}")
                print(f"    Context: ...{change['context']}...")
            total_changes += len(changes)
        else:
            print(f"  No changes")
    
    print("\n" + "=" * 70)
    if args.dry_run:
        print(f"Total changes: {total_changes}")
        print("Run with --apply to make changes")
    else:
        print(f"Applied {total_changes} changes")
    print("=" * 70)

if __name__ == "__main__":
    main()
