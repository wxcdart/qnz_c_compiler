#!/usr/bin/env python3
"""Stack Offset Analyzer.

Extracts and validates stack frame offsets from generated object files.
Helps identify incorrect stack allocation that could cause variable overlap.

Usage:
    python3 tools/stack_offset_analyzer.py /path/to/object.o
"""

import subprocess
import sys
import re
from pathlib import Path
from dataclasses import dataclass
from typing import Optional

@dataclass
class StackAccess:
    func_name: str
    offset: int
    instruction: str
    access_type: str  # 'store', 'load', 'alloc'

def extract_stack_accesses(obj_path: Path) -> list[StackAccess]:
    """Extract all stack accesses from disassembly."""
    result = subprocess.run(
        ['objdump', '-d', str(obj_path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    
    accesses = []
    current_func = None
    
    # Patterns for stack accesses
    store_pattern = re.compile(r'mov\s+\w+\[(?:rbp|fp)([-+])0x([0-9a-f]+),\s+\w+')
    load_pattern = re.compile(r'mov\s+\w+,\s+\w+\[(?:rbp|fp)([-+])0x([0-9a-f]+)')
    alloc_pattern = re.compile(r'sub\s+\w+,\s+\$0x([0-9a-f]+)')
    
    for line in result.stdout.splitlines():
        # Track current function
        func_match = re.match(r'\s*\d+ <([^>]+)>:', line)
        if func_match:
            current_func = func_match.group(1)
            continue
        
        if not current_func:
            continue
        
        # Check for stack allocation
        alloc_match = alloc_pattern.search(line)
        if alloc_match:
            size = int(alloc_match.group(1), 16)
            accesses.append(StackAccess(
                func_name=current_func,
                offset=-size,
                instruction=line.strip(),
                access_type='alloc'
            ))
        
        # Check for stack stores
        store_match = store_pattern.search(line)
        if store_match:
            sign = store_match.group(1)
            offset_hex = store_match.group(2)
            offset = int(offset_hex, 16)
            if sign == '-':
                offset = -offset
            accesses.append(StackAccess(
                func_name=current_func,
                offset=offset,
                instruction=line.strip(),
                access_type='store'
            ))
        
        # Check for stack loads
        load_match = load_pattern.search(line)
        if load_match:
            sign = load_match.group(1)
            offset_hex = load_match.group(2)
            offset = int(offset_hex, 16)
            if sign == '-':
                offset = -offset
            accesses.append(StackAccess(
                func_name=current_func,
                offset=offset,
                instruction=line.strip(),
                access_type='load'
            ))
    
    return accesses

def analyze_stack_layout(accesses: list[StackAccess]) -> dict[str, dict]:
    """Analyze stack layout per function."""
    func_layouts = {}
    
    for access in accesses:
        if access.func_name not in func_layouts:
            func_layouts[access.func_name] = {
                'alloc_size': 0,
                'min_offset': 0,
                'max_offset': 0,
                'accesses': []
            }
        
        layout = func_layouts[access.func_name]
        
        if access.access_type == 'alloc':
            layout['alloc_size'] = abs(access.offset)
        else:
            layout['min_offset'] = min(layout['min_offset'], access.offset)
            layout['max_offset'] = max(layout['max_offset'], access.offset)
            layout['accesses'].append(access)
    
    return func_layouts

def detect_overlap(layouts: dict[str, dict]) -> list[tuple[str, int, int]]:
    """Detect potential stack variable overlaps."""
    overlaps = []
    
    for func_name, layout in layouts.items():
        if layout['alloc_size'] == 0:
            continue
        
        # Check if any access goes beyond allocated space
        for access in layout['accesses']:
            if access.offset < -layout['alloc_size']:
                overlaps.append((
                    func_name,
                    access.offset,
                    -layout['alloc_size']
                ))
    
    return overlaps

def main() -> int:
    import argparse
    parser = argparse.ArgumentParser(description='Analyze stack offsets')
    parser.add_argument('object_file', type=Path, help='Object file to analyze')
    parser.add_argument('--detect-overlap', action='store_true',
                       help='Detect potential stack variable overlaps')
    args = parser.parse_args()
    
    if not args.object_file.exists():
        print(f"[error] Object file not found: {args.object_file}")
        return 1
    
    print(f"Analyzing {args.object_file}...")
    
    accesses = extract_stack_accesses(args.object_file)
    layouts = analyze_stack_layout(accesses)
    
    print(f"\n=== Stack Layout Analysis ===")
    print(f"Functions analyzed: {len(layouts)}")
    print(f"Total stack accesses: {len(accesses)}")
    
    print(f"\nPer-function summary:")
    print(f"{'Function':<30} {'Alloc Size':<15} {'Min Offset':<15} {'Max Offset':<15}")
    print("-" * 75)
    
    for func_name, layout in sorted(layouts.items()):
        print(f"{func_name:<30} {layout['alloc_size']:<15} {layout['min_offset']:<15} {layout['max_offset']:<15}")
    
    if args.detect_overlap:
        overlaps = detect_overlap(layouts)
        if overlaps:
            print(f"\n[WARNING] Potential stack overlaps detected:")
            for func, offset, limit in overlaps:
                print(f"  {func}: access at {offset} exceeds allocation limit {limit}")
            return 1
        else:
            print(f"\n[PASS] No stack overlaps detected")
    
    return 0

if __name__ == '__main__':
    sys.exit(main())
