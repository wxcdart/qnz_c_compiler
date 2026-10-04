#!/usr/bin/env python3
"""
Verify Task 61 completion - check all globals are replaced.
Usage: python3 tools/verify_task61.py
"""

import os
import re
import glob

# Globals that should NOT exist after Task 61
FORBIDDEN_GLOBALS = [
    'g_ir_emitter_ctx',
    'g_ir_epilogue_label',
    'g_loop_label_stack',
    'g_loop_label_depth',
    'g_user_labels',
    'g_user_label_count',
]

def check_file(filepath):
    """Check if file still uses forbidden globals."""
    issues = []
    try:
        with open(filepath, 'r') as f:
            content = f.read()
        
        for global_name in FORBIDDEN_GLOBALS:
            # Look for usage (not definition)
            pattern = r'\b' + re.escape(global_name) + r'\b'
            matches = list(re.finditer(pattern, content))
            
            for match in matches:
                line_num = content[:match.start()].count('\n') + 1
                line_start = content.rfind('\n', 0, match.start()) + 1
                line_end = content.find('\n', match.end())
                if line_end == -1:
                    line_end = len(content)
                line = content[line_start:line_end].strip()
                
                # Skip if it's a definition (static ... g_...)
                if re.search(r'static\s+\w+\s+' + re.escape(global_name), line):
                    continue
                
                issues.append({
                    'file': filepath,
                    'line': line_num,
                    'global': global_name,
                    'code': line
                })
    except Exception as e:
        issues.append({'file': filepath, 'error': str(e)})
    
    return issues

def main():
    print("=" * 70)
    print("Task 61 Verification: Check for Remaining Globals")
    print("=" * 70)
    
    all_issues = []
    for filepath in glob.glob('src/**/*.h', recursive=True) + glob.glob('src/**/*.c', recursive=True):
        issues = check_file(filepath)
        all_issues.extend(issues)
    
    if all_issues:
        print(f"\n❌ Found {len(all_issues)} remaining global references:")
        for issue in all_issues:
            if 'error' in issue:
                print(f"  ⚠️  {issue['file']}: {issue['error']}")
            else:
                print(f"  {issue['file']}:{issue['line']}")
                print(f"    {issue['global']} → should be c->lowering.*")
                print(f"    {issue['code'][:80]}")
        print("\n" + "=" * 70)
        print("Task 61 INCOMPLETE - globals still in use")
        print("=" * 70)
        return 1
    else:
        print("\n✅ All forbidden globals have been replaced!")
        print("\n" + "=" * 70)
        print("Task 61 COMPLETE - no remaining global references")
        print("=" * 70)
        return 0

if __name__ == "__main__":
    exit(main())
