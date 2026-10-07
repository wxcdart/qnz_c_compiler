#!/usr/bin/env python3
"""
Stack Offset Tracer - Traces stack offset assignments during compilation

This tool helps identify the frontend bug where stack offsets are incorrectly
assigned for variables in if/else branches.

Usage:
  python3 stack_offset_tracer.py --source test.c --output trace.json
  python3 stack_offset_tracer.py --analyze trace.json
"""

import subprocess
import re
import sys
import json
import argparse
from dataclasses import dataclass, asdict
from typing import List, Optional, Dict, Set
from collections import defaultdict

@dataclass
class LocalVar:
    """Represents a local variable"""
    name: str
    type: str
    stack_offset: int
    function: str
    line: int = 0

@dataclass
class FunctionFrame:
    """Represents a function's stack frame"""
    name: str
    frame_size: int
    locals: List[LocalVar]
    max_offset: int
    min_offset: int
    offset_gaps: List[int]  # Gaps between consecutive offsets

def analyze_objdump(objdump_output: str) -> Dict[str, FunctionFrame]:
    """Analyze objdump output to extract stack frame information"""
    functions = {}
    current_func = None
    current_locals = []
    frame_size = 0
    
    lines = objdump_output.split('\n')
    
    for line in lines:
        # Detect function start
        func_match = re.match(r'(\d+) <([^>]+)>:', line)
        if func_match:
            # Save previous function
            if current_func:
                if current_locals:
                    offsets = [l['offset'] for l in current_locals]
                    functions[current_func] = {
                        'frame_size': frame_size,
                        'locals': current_locals,
                        'max_offset': max(offsets) if offsets else 0,
                        'min_offset': min(offsets) if offsets else 0,
                        'offset_gaps': calculate_gaps(offsets)
                    }
            
            current_func = func_match.group(2)
            current_locals = []
            frame_size = 0
            continue
        
        if not current_func:
            continue
        
        # Detect frame size
        size_match = re.search(r'sub\s+(?:sp|rsp),\s*(?:sp|rsp),\s*#?(\d+|0x[0-9a-f]+)', line, re.IGNORECASE)
        if size_match:
            size_str = size_match.group(1)
            frame_size = int(size_str, 16) if size_str.startswith('0x') else int(size_str)
        
        # Detect stack accesses (these are the actual offsets used)
        access_match = re.search(r'\[(?:x29|rbp),\s*(#?-?\d+)\]', line)
        if access_match:
            offset_str = access_match.group(1)
            offset = int(offset_str) if not offset_str.startswith('#') else int(offset_str[1:])
            
            # Determine if load or store
            is_store = 'stur' in line.lower() or 'str' in line.lower() or 'mov' in line.lower() and 'store' in line.lower()
            access_type = 'store' if is_store else 'load'
            
            # Try to extract variable name from context (this is heuristic)
            var_name = f"var_at_{offset}"
            
            current_locals.append({
                'offset': offset,
                'access_type': access_type,
                'instruction': line.strip()
            })
    
    # Save last function
    if current_func and current_locals:
        offsets = [l['offset'] for l in current_locals]
        functions[current_func] = {
            'frame_size': frame_size,
            'locals': current_locals,
            'max_offset': max(offsets) if offsets else 0,
            'min_offset': min(offsets) if offsets else 0,
            'offset_gaps': calculate_gaps(offsets)
        }
    
    return functions

def calculate_gaps(offsets: List[int]) -> List[int]:
    """Calculate gaps between consecutive stack offsets"""
    if len(offsets) < 2:
        return []
    
    sorted_offsets = sorted(set(offsets))
    gaps = []
    
    for i in range(1, len(sorted_offsets)):
        gap = sorted_offsets[i] - sorted_offsets[i-1]
        if gap != 8:  # Normal gap is 8 bytes
            gaps.append(gap)
    
    return gaps

def detect_if_else_bug(functions: Dict[str, FunctionFrame]) -> List[Dict]:
    """Detect potential if/else stack offset bugs"""
    bugs = []
    
    for func_name, frame in functions.items():
        offsets = [l['offset'] for l in frame['locals']]
        
        # Look for suspicious patterns:
        # 1. Offset -8 in a large frame (likely wrong)
        if -8 in offsets and frame['frame_size'] > 32:
            bugs.append({
                'function': func_name,
                'type': 'suspicious_offset',
                'description': f"Offset -8 detected in {frame['frame_size']}-byte frame",
                'offsets': offsets,
                'frame_size': frame['frame_size']
            })
        
        # 2. Large gaps in offsets (might indicate offset reuse bug)
        if frame['offset_gaps']:
            for gap in frame['offset_gaps']:
                if gap > 16:  # Unusual gap
                    bugs.append({
                        'function': func_name,
                        'type': 'large_gap',
                        'description': f"Unusual gap of {gap} bytes between offsets",
                        'offsets': offsets,
                        'gap': gap
                    })
        
        # 3. Check if max_offset is close to frame_size (good) vs far from it (might be bug)
        if abs(frame['max_offset']) < frame['frame_size'] / 2:
            bugs.append({
                'function': func_name,
                'type': 'underutilized_frame',
                'description': f"Frame size {frame['frame_size']} but max offset is only {frame['max_offset']}",
                'offsets': offsets,
                'frame_size': frame['frame_size'],
                'max_offset': frame['max_offset']
            })
    
    return bugs

def generate_report(functions: Dict[str, FunctionFrame], bugs: List[Dict]) -> str:
    """Generate a human-readable report"""
    lines = []
    lines.append("=" * 70)
    lines.append("Stack Offset Analysis Report")
    lines.append("=" * 70)
    lines.append("")
    
    for func_name, frame in functions.items():
        lines.append(f"Function: {func_name}")
        lines.append(f"  Frame size: {frame['frame_size']} bytes")
        lines.append(f"  Offsets used: {sorted(set([l['offset'] for l in frame['locals']]))}")
        lines.append(f"  Max offset: {frame['max_offset']}, Min offset: {frame['min_offset']}")
        
        if frame['offset_gaps']:
            lines.append(f"  Unusual gaps: {frame['offset_gaps']}")
        
        lines.append("")
    
    if bugs:
        lines.append("=" * 70)
        lines.append("POTENTIAL BUGS DETECTED")
        lines.append("=" * 70)
        lines.append("")
        
        for bug in bugs:
            lines.append(f"[{bug['type'].upper()}] {bug['function']}")
            lines.append(f"  {bug['description']}")
            lines.append("")
    
    return "\n".join(lines)

def main():
    parser = argparse.ArgumentParser(description='Trace stack offset assignments')
    parser.add_argument('--object-file', help='Object file to analyze')
    parser.add_argument('--analyze', help='Analyze a previously generated trace JSON file')
    parser.add_argument('--output', help='Output file for trace JSON')
    parser.add_argument('--verbose', '-v', action='store_true', help='Verbose output')
    
    args = parser.parse_args()
    
    if args.analyze:
        # Load and analyze existing trace
        with open(args.analyze, 'r') as f:
            data = json.load(f)
        
        functions = {k: v for k, v in data['functions'].items()}
        bugs = detect_if_else_bug(functions)
        report = generate_report(functions, bugs)
        print(report)
        return 1 if bugs else 0
    
    if not args.object_file:
        parser.error("Must specify --object-file or --analyze")
    
    # Determine target architecture from filename or default to aarch64
    target = 'aarch64'
    if 'x86' in args.object_file or 'x64' in args.object_file:
        target = 'x86_64'
    
    # Run objdump
    try:
        result = subprocess.run(
            [f'{target}-linux-gnu-objdump', '-d', args.object_file],
            capture_output=True, text=True, check=True
        )
    except subprocess.CalledProcessError as e:
        print(f"Error running objdump: {e}")
        sys.exit(1)
    
    # Analyze
    functions = analyze_objdump(result.stdout)
    bugs = detect_if_else_bug(functions)
    
    # Generate report
    report = generate_report(functions, bugs)
    print(report)
    
    # Save trace if requested
    if args.output:
        trace_data = {
            'object_file': args.object_file,
            'target': target,
            'functions': functions,
            'bugs': bugs
        }
        with open(args.output, 'w') as f:
            json.dump(trace_data, f, indent=2)
        print(f"\nTrace saved to: {args.output}")
    
    return 1 if bugs else 0

if __name__ == '__main__':
    sys.exit(main())
