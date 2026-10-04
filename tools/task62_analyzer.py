#!/usr/bin/env python3
"""
Task 62: Unified Label & Branch Patching Analysis.
Identifies all label/patching code paths.
Usage: python3 tools/task62_analyzer.py
"""

import re
import glob

def analyze_file(filepath):
    """Analyze file for label/patching patterns."""
    patterns = {
        'label_emit': r'ir_emit_label|emit_label',
        'label_patch': r'patch.*label|label.*patch',
        'epilogue': r'epilogue',
        'goto_patch': r'goto.*patch|patch.*goto',
        'break_continue': r'break.*patch|continue.*patch',
        'resolve_labels': r'resolve.*label',
    }
    
    findings = {}
    try:
        with open(filepath, 'r') as f:
            lines = f.readlines()
        
        for pattern_name, pattern in patterns.items():
            for i, line in enumerate(lines, 1):
                if re.search(pattern, line, re.IGNORECASE):
                    if pattern_name not in findings:
                        findings[pattern_name] = []
                    findings[pattern_name].append({
                        'line': i,
                        'code': line.strip()[:100]
                    })
    except Exception as e:
        findings['error'] = str(e)
    
    return findings

def main():
    print("=" * 70)
    print("Task 62: Unified Label & Branch Patching Analysis")
    print("=" * 70)
    
    all_findings = {}
    for filepath in glob.glob('src/**/*.h', recursive=True) + glob.glob('src/**/*.c', recursive=True):
        findings = analyze_file(filepath)
        if any(v for v in findings.values() if isinstance(v, list) and v):
            all_findings[filepath] = findings
    
    # Group by pattern type
    by_pattern = {}
    for filepath, findings in all_findings.items():
        for pattern_name, matches in findings.items():
            if isinstance(matches, list) and matches:
                if pattern_name not in by_pattern:
                    by_pattern[pattern_name] = []
                for match in matches:
                    by_pattern[pattern_name].append({
                        'file': filepath,
                        'line': match['line'],
                        'code': match['code']
                    })
    
    for pattern_name in sorted(by_pattern.keys()):
        print(f"\n{pattern_name}: ({len(by_pattern[pattern_name])} occurrences)")
        for match in by_pattern[pattern_name][:10]:  # Show first 10
            print(f"  {match['file']}:{match['line']}")
            print(f"    {match['code']}")
        if len(by_pattern[pattern_name]) > 10:
            print(f"  ... and {len(by_pattern[pattern_name]) - 10} more")
    
    print("\n" + "=" * 70)
    print("Files needing Task 62 work:")
    for filepath in sorted(all_findings.keys()):
        print(f"  - {filepath}")
    print("=" * 70)

if __name__ == "__main__":
    main()
