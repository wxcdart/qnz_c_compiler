#!/usr/bin/env python3
"""
Stack Frame Analyzer - Analyzes prologue/epilogue patterns and stack frame layout

This tool helps verify:
1. Prologue/epilogue pattern consistency (Pattern A vs Pattern B)
2. Stack frame size calculation correctness
3. Stack offset assignments for local variables
4. Disjoint lexical scope handling (if/else branches)
"""

import subprocess
import re
import sys
import argparse
from dataclasses import dataclass
from typing import List, Optional, Tuple

@dataclass
class StackFrame:
    """Represents a function's stack frame layout"""
    name: str
    frame_size: int
    prologue_pattern: str  # "Pattern A" or "Pattern B"
    saved_fp_lr_offset: int  # Offset where FP/LR are saved
    locals_start_offset: int  # Where local variables start
    prologue_instructions: List[str]
    epilogue_instructions: List[str]
    max_offset_used: int  # Maximum stack offset accessed (for debugging)

@dataclass
class StackAccess:
    """Represents a stack memory access"""
    offset: int  # Negative offset from FP
    instruction: str
    access_type: str  # "load" or "store"
    address: int  # Instruction address

def parse_objdump(output: str) -> List[StackFrame]:
    """Parse objdump output to extract stack frame information"""
    frames = []
    current_func = None
    prologue = []
    epilogue = []
    in_prologue = False
    in_epilogue = False
    
    lines = output.split('\n')
    i = 0
    
    while i < len(lines):
        line = lines[i]
        
        # Detect function start
        func_match = re.match(r'(\d+) <([^>]+)>:', line)
        if func_match:
            # Save previous function
            if current_func:
                frames.append(current_func)
            
            current_func = {
                'name': func_match.group(2),
                'prologue': [],
                'epilogue': [],
                'stack_accesses': [],
                'frame_size': 0,
                'pattern': None
            }
            in_prologue = True
            in_epilogue = False
            i += 1
            continue
        
        if not current_func:
            i += 1
            continue
        
        # Parse instruction
        instr_match = re.match(r'\s*([0-9a-f]+):\s+([0-9a-f\s]+)\s+(.+)', line)
        if instr_match:
            addr = instr_match.group(1)
            hex_bytes = instr_match.group(2)
            asm = instr_match.group(3).strip()
            
            # Detect prologue patterns
            if in_prologue:
                if 'sub' in asm and 'sp' in asm and '#' in asm:
                    # Extract frame size
                    size_match = re.search(r'#(0x[0-9a-f]+|\d+)', asm)
                    if size_match:
                        size_str = size_match.group(1)
                        if size_str.startswith('0x'):
                            current_func['frame_size'] = int(size_str, 16)
                        else:
                            current_func['frame_size'] = int(size_str)
                
                if 'stp' in asm and 'x29' in asm and 'x30' in asm:
                    # Determine pattern
                    if '[sp, #' in asm and '-16]!' in asm:
                        current_func['pattern'] = 'Pattern A'
                        current_func['saved_fp_lr_offset'] = -16
                    elif '[sp]' in asm or '[sp, #0]' in asm:
                        current_func['pattern'] = 'Pattern B'
                        current_func['saved_fp_lr_offset'] = 0
                
                prologue.append(f"{addr}: {asm}")
                
                # End of prologue detection
                if 'mov' in asm and 'x29, sp' in asm:
                    in_prologue = False
                    continue
            
            # Detect epilogue
            if 'ldp' in asm and 'x29' in asm and 'x30' in asm:
                in_epilogue = True
            
            if in_epilogue:
                epilogue.append(f"{addr}: {asm}")
                
                # End of epilogue detection
                if 'ret' in asm:
                    current_func['epilogue'] = epilogue
                    epilogue = []
                    in_epilogue = False
            
            # Track stack accesses
            offset_match = re.search(r'\[x29, #(.-?\d+)\]', asm)
            if offset_match:
                offset_str = offset_match.group(1)
                offset = int(offset_str)
                access_type = 'store' if 'stur' in asm or 'str' in asm else 'load'
                current_func['stack_accesses'].append({
                    'offset': offset,
                    'instruction': asm,
                    'access_type': access_type,
                    'address': addr
                })
        
        i += 1
    
    # Save last function
    if current_func:
        frames.append(current_func)
    
    return frames

def analyze_frame(frame: StackFrame) -> dict:
    """Analyze a single stack frame for issues"""
    issues = []
    warnings = []
    
    # Check for Pattern A/B consistency
    if frame['pattern'] == 'Pattern A':
        # Pattern A: stp with [sp, #-16]! then mov x29,sp then sub sp,sp,#size
        # x29 points to saved FP/LR, locals at negative offsets
        expected_first_offset = -8  # First local after saved FP/LR
    elif frame['pattern'] == 'Pattern B':
        # Pattern B: sub sp,sp,#size then stp [sp] then mov x29,sp
        # x29 points to frame bottom, locals should be at positive offsets
        # But compiler uses negative offsets (frontend bug)
        expected_first_offset = -8  # Compiler convention
    else:
        issues.append("Unknown prologue pattern")
        return {'issues': issues, 'warnings': warnings}
    
    # Check stack offset consistency
    if frame['stack_accesses']:
        offsets = [a['offset'] for a in frame['stack_accesses']]
        min_offset = min(offsets)
        max_offset = max(offsets)
        
        # Check if any offset exceeds frame size
        if abs(min_offset) > frame['frame_size']:
            issues.append(f"Stack offset {min_offset} exceeds frame size {frame['frame_size']}")
        
        # Check for overlapping offsets in if/else branches
        # This is the key bug we're looking for
        if len(set(offsets)) != len(offsets):
            warnings.append(f"Duplicate stack offsets detected: {offsets}")
        
        # Check for suspicious offset values (like -8 which might be wrong)
        if -8 in offsets and frame['frame_size'] > 32:
            warnings.append(f"Stack offset -8 detected in large frame ({frame['frame_size']} bytes) - possible frontend bug")
    
    return {'issues': issues, 'warnings': warnings}

def compare_isa_frames(frames_x86: List[dict], frames_aarch64: List[dict]) -> List[dict]:
    """Compare stack frames across ISAs to find discrepancies"""
    comparisons = []
    
    for x86_frame in frames_x86:
        for aarch64_frame in frames_aarch64:
            if x86_frame['name'] == aarch64_frame['name']:
                # Compare frame sizes
                size_diff = abs(x86_frame['frame_size'] - aarch64_frame['frame_size'])
                if size_diff > 16:
                    comparisons.append({
                        'function': x86_frame['name'],
                        'issue': f"Frame size mismatch: x86={x86_frame['frame_size']}, aarch64={aarch64_frame['frame_size']}"
                    })
                
                # Compare stack offsets used
                x86_offsets = set([a['offset'] for a in x86_frame.get('stack_accesses', [])])
                aarch64_offsets = set([a['offset'] for a in aarch64_frame.get('stack_accesses', [])])
                
                if x86_offsets != aarch64_offsets:
                    only_x86 = x86_offsets - aarch64_offsets
                    only_aarch64 = aarch64_offsets - x86_offsets
                    if only_aarch64:
                        comparisons.append({
                            'function': x86_frame['name'],
                            'issue': f"AArch64 uses offsets not in x86: {only_aarch64}"
                        })
    
    return comparisons

def main():
    parser = argparse.ArgumentParser(description='Analyze stack frame layout')
    parser.add_argument('object_file', help='Object file to analyze')
    parser.add_argument('--target', choices=['aarch64', 'x86_64'], default='aarch64',
                       help='Target architecture')
    parser.add_argument('--compare', help='Compare with another object file (different ISA)')
    parser.add_argument('--verbose', '-v', action='store_true', help='Verbose output')
    
    args = parser.parse_args()
    
    # Run objdump
    try:
        result = subprocess.run(
            [f'{args.target}-linux-gnu-objdump', '-d', args.object_file],
            capture_output=True, text=True, check=True
        )
    except subprocess.CalledProcessError as e:
        print(f"Error running objdump: {e}")
        sys.exit(1)
    
    # Parse output
    frames = parse_objdump(result.stdout)
    
    # Analyze each frame
    print(f"\n{'='*70}")
    print(f"Stack Frame Analysis: {args.object_file}")
    print(f"{'='*70}\n")
    
    all_issues = []
    all_warnings = []
    
    for frame in frames:
        print(f"Function: {frame['name']}")
        print(f"  Frame size: {frame['frame_size']} bytes")
        print(f"  Pattern: {frame['pattern']}")
        print(f"  Saved FP/LR offset: {frame['saved_fp_lr_offset']}")
        
        if frame['stack_accesses']:
            offsets = sorted([a['offset'] for a in frame['stack_accesses']])
            print(f"  Stack offsets used: {offsets}")
            print(f"  Min offset: {min(offsets)}, Max offset: {max(offsets)}")
        
        analysis = analyze_frame(frame)
        
        if analysis['issues']:
            print(f"  ISSUES:")
            for issue in analysis['issues']:
                print(f"    - {issue}")
            all_issues.extend(analysis['issues'])
        
        if analysis['warnings']:
            print(f"  WARNINGS:")
            for warning in analysis['warnings']:
                print(f"    - {warning}")
            all_warnings.extend(analysis['warnings'])
        
        print()
    
    # Compare with other ISA if requested
    if args.compare:
        print(f"\n{'='*70}")
        print(f"Cross-ISA Comparison")
        print(f"{'='*70}\n")
        
        try:
            result2 = subprocess.run(
                ['objdump', '-d', args.compare],
                capture_output=True, text=True, check=True
            )
            frames2 = parse_objdump(result2.stdout)
            comparisons = compare_isa_frames(frames, frames2)
            
            if comparisons:
                for comp in comparisons:
                    print(f"{comp['function']}: {comp['issue']}")
            else:
                print("No discrepancies found between ISAs")
        except subprocess.CalledProcessError as e:
            print(f"Error comparing: {e}")
    
    # Summary
    print(f"\n{'='*70}")
    print(f"Summary")
    print(f"{'='*70}")
    print(f"Functions analyzed: {len(frames)}")
    print(f"Issues found: {len(all_issues)}")
    print(f"Warnings found: {len(all_warnings)}")
    
    if all_issues:
        print(f"\nIssues:")
        for issue in all_issues:
            print(f"  - {issue}")
    
    if all_warnings:
        print(f"\nWarnings:")
        for warning in all_warnings:
            print(f"  - {warning}")
    
    return len(all_issues) > 0

if __name__ == '__main__':
    sys.exit(1 if main() else 0)
