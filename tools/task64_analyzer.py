#!/usr/bin/env python3
"""
Task 64: Target-Independent IR Canonicalization Analysis.
Identifies ISA-specific patterns in IR layer.
Usage: python3 tools/task64_analyzer.py
"""

import re
import glob

# ISA-specific patterns that should be in backend, not IR layer
ISA_PATTERNS = {
    'x86_specific': [
        r'rax|rbx|rcx|rdx|rsi|rdi|r[8-9][0-9]?',  # x86 registers
        r'xmm[0-9]+',  # x86 FP registers
        r'sub.*rsp', r'mov.*rsp',  # x86 stack
        r'push.*rb', r'pop.*rb',  # x86 push/pop
    ],
    'aarch64_specific': [
        r'x[0-9]+|w[0-9]+',  # AArch64 registers
        r'v[0-9]+',  # AArch64 FP registers
        r'sp\b',  # stack pointer
        r'blr', r'blr\s+x',  # AArch64 call
    ],
    'isa_backend_calls': [
        r'BE\(c\)->',  # Backend macro
        r'backend->',
    ],
    'machine_code_emit': [
        r'emit_bytes', r'emit_u8', r'emit_u16', r'emit_u32', r'emit_u64',
        r'emit_.*rel32', r'emit_.*imm',
    ],
}

def analyze_file(filepath):
    """Analyze file for ISA-specific patterns."""
    findings = {}
    try:
        with open(filepath, 'r') as f:
            lines = f.readlines()
        
        for pattern_name, patterns in ISA_PATTERNS.items():
            for pattern in patterns:
                for i, line in enumerate(lines, 1):
                    if re.search(pattern, line):
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
    print("Task 64: Target-Independent IR Canonicalization Analysis")
    print("=" * 70)
    
    # Only check parser/ directory (IR layer, not ISA backends)
    files_to_check = (
        glob.glob('src/zero_cc/parser/*.h', recursive=True) +
        glob.glob('src/zero_cc/parser/*.c', recursive=True)
    )
    
    all_findings = {}
    for filepath in files_to_check:
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
        for match in by_pattern[pattern_name][:5]:  # Show first 5
            print(f"  {match['file']}:{match['line']}")
            print(f"    {match['code']}")
        if len(by_pattern[pattern_name]) > 5:
            print(f"  ... and {len(by_pattern[pattern_name]) - 5} more")
    
    print("\n" + "=" * 70)
    print("Files with ISA-specific patterns in IR layer:")
    for filepath in sorted(all_findings.keys()):
        print(f"  - {filepath}")
    print("\nRecommendation for Task 64:")
    print("  - Move ISA-specific emit_* calls to backend layer")
    print("  - Create canonical IR ops (e.g., OP_PUSH_FRAME, OP_ALLOC_STACK)")
    print("  - Ensure IR layer is target-agnostic")
    print("=" * 70)

if __name__ == "__main__":
    main()
