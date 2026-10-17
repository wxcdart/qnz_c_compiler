#!/usr/bin/env python3
"""
Stack Frame Layout Analyzer

Analyzes stack frame layout across ISAs to detect inconsistencies
and potential bugs in frame pointer usage.

Usage:
    python3 stack_frame_layout_analyzer.py --object-file test.o --target aarch64
    python3 stack_frame_layout_analyzer.py --compare x86_test.o aarch64_test.o
"""

import argparse
import subprocess
import re
import sys
import json
from dataclasses import dataclass, field
from typing import List, Dict, Optional, Set
from pathlib import Path

@dataclass
class StackAccess:
    """Represents a stack memory access"""
    offset: int
    instruction: str
    address: int
    access_type: str  # "load" or "store"
    base_reg: str

@dataclass
class FunctionFrame:
    """Represents a function's stack frame"""
    name: str
    frame_size: int
    prologue_pattern: str  # "Pattern A" or "Pattern B" or "Unknown"
    saved_fp_lr_offset: int
    stack_accesses: List[StackAccess] = field(default_factory=list)
    max_offset: int = 0
    min_offset: int = 0
    prologue_instructions: List[str] = field(default_factory=list)
    epilogue_instructions: List[str] = field(default_factory=list)

@dataclass
class FrameAnalysis:
    """Complete frame analysis result"""
    functions: Dict[str, FunctionFrame] = field(default_factory=dict)
    issues: List[str] = field(default_factory=list)
    warnings: List[str] = field(default_factory=list)
    target_isa: str = ""

class StackFrameAnalyzer:
    """Analyzes stack frame layout from objdump output"""
    
    def __init__(self, objdump_output: str, target_isa: str = "aarch64"):
        self.objdump_output = objdump_output
        self.target_isa = target_isa
        self.functions: Dict[str, FunctionFrame] = {}
        
    def analyze(self) -> FrameAnalysis:
        """Parse objdump and analyze stack frames"""
        lines = self.objdump_output.split('\n')
        current_func = None
        prologue = []
        epilogue = []
        in_prologue = False
        in_epilogue = False
        frame_size = 0
        pattern = None
        saved_fp_lr_offset = 0
        
        for line in lines:
            # Detect function start
            func_match = re.match(r'\s*(\d+):\s+<([^>]+)>:', line)
            if func_match:
                # Save previous function
                if current_func:
                    self._save_function(current_func, prologue, epilogue, 
                                       frame_size, pattern, saved_fp_lr_offset)
                
                current_func = func_match.group(2)
                prologue = []
                epilogue = []
                in_prologue = True
                in_epilogue = False
                frame_size = 0
                pattern = None
                saved_fp_lr_offset = 0
                continue
            
            if not current_func:
                continue
            
            # Parse instruction
            instr_match = re.match(r'\s*[0-9a-f]+:\s+([0-9a-f\s]+)\s+(.+)', line)
            if instr_match:
                hex_bytes = instr_match.group(1)
                asm = instr_match.group(2).strip()
                
                # Detect prologue
                if in_prologue:
                    prologue.append(asm)
                    
                    # Detect frame size
                    if self.target_isa == "aarch64":
                        size_match = re.search(r'sub\s+sp,\s+sp,\s*#(0x[0-9a-f]+|\d+)', asm)
                        if size_match:
                            size_str = size_match.group(1)
                            frame_size = int(size_str, 16) if size_str.startswith('0x') else int(size_str)
                        
                        # Detect pattern
                        if 'stp' in asm and 'x29' in asm and 'x30' in asm:
                            if '[sp, #' in asm and '-16]!' in asm:
                                pattern = "Pattern A"
                                saved_fp_lr_offset = -16
                            elif '[sp]' in asm or '[sp, #0]' in asm:
                                pattern = "Pattern B"
                                saved_fp_lr_offset = 0
                        
                        # End of prologue
                        if 'mov' in asm and 'x29, sp' in asm:
                            in_prologue = False
                
                elif self.target_isa == "x86_64":
                    prologue.append(asm)
                    
                    # Detect frame size
                    size_match = re.search(r'sub\s+\$?(0x[0-9a-f]+|\d+),\s*%rsp', asm)
                    if size_match:
                        size_str = size_match.group(1)
                        frame_size = int(size_str, 16) if size_str.startswith('0x') else int(size_str)
                    
                    # Pattern detection for x86-64
                    if 'push' in asm and 'rbp' in asm:
                        pattern = "x86-64 standard"
                        saved_fp_lr_offset = 0
                    
                    # End of prologue
                    if 'mov' in asm and '%rsp,%rbp' in asm:
                        in_prologue = False
                
                # Detect stack accesses
                access_match = re.search(r'\[(x29|rbp),\s*(#?-?\d+)\]', asm)
                if access_match:
                    base_reg = access_match.group(1)
                    offset_str = access_match.group(2)
                    offset = int(offset_str) if not offset_str.startswith('#') else int(offset_str[1:])
                    
                    access_type = 'store' if any(kw in asm for kw in ['stur', 'str', 'mov']) else 'load'
                    
                    access = StackAccess(
                        offset=offset,
                        instruction=asm,
                        address=0,  # Would need more parsing
                        access_type=access_type,
                        base_reg=base_reg
                    )
                    
                    if current_func not in self.functions:
                        self.functions[current_func] = FunctionFrame(
                            name=current_func,
                            frame_size=frame_size,
                            prologue_pattern=pattern or "Unknown",
                            saved_fp_lr_offset=saved_fp_lr_offset
                        )
                    
                    self.functions[current_func].stack_accesses.append(access)
                
                # Detect epilogue
                if 'ldp' in asm and 'x29' in asm and 'x30' in asm:
                    in_epilogue = True
                
                if in_epilogue:
                    epilogue.append(asm)
                    if 'ret' in asm:
                        in_epilogue = False
        
        # Save last function
        if current_func:
            self._save_function(current_func, prologue, epilogue,
                               frame_size, pattern, saved_fp_lr_offset)
        
        return self._generate_analysis()
    
    def _save_function(self, name: str, prologue: List[str], epilogue: List[str],
                      frame_size: int, pattern: str, saved_fp_lr_offset: int):
        """Save function frame information"""
        if name not in self.functions:
            self.functions[name] = FunctionFrame(
                name=name,
                frame_size=frame_size,
                prologue_pattern=pattern or "Unknown",
                saved_fp_lr_offset=saved_fp_lr_offset,
                prologue_instructions=prologue,
                epilogue_instructions=epilogue
            )
    
    def _generate_analysis(self) -> FrameAnalysis:
        """Generate analysis report"""
        issues = []
        warnings = []
        
        for name, frame in self.functions.items():
            # Calculate offset range
            if frame.stack_accesses:
                offsets = [a.offset for a in frame.stack_accesses]
                frame.max_offset = max(offsets)
                frame.min_offset = min(offsets)
                
                # Check for suspicious offsets
                if -8 in offsets and frame.frame_size > 32:
                    warnings.append(
                        f"{name}: Offset -8 detected in {frame.frame_size}-byte frame. "
                        "This may indicate callee-saved register overlap bug."
                    )
                
                # Check for offsets exceeding frame size
                if abs(frame.min_offset) > frame.frame_size:
                    issues.append(
                        f"{name}: Stack offset {frame.min_offset} exceeds frame size {frame.frame_size}"
                    )
                
                # Check for large gaps
                sorted_offsets = sorted(set(offsets))
                for i in range(1, len(sorted_offsets)):
                    gap = sorted_offsets[i] - sorted_offsets[i-1]
                    if gap > 16 and gap < frame.frame_size:
                        warnings.append(
                            f"{name}: Large gap of {gap} bytes between offsets "
                            f"{sorted_offsets[i-1]} and {sorted_offsets[i]}"
                        )
            
            # Check prologue/epilogue consistency
            if frame.prologue_pattern == "Pattern A" and frame.epilogue_instructions:
                epilogue_str = ' '.join(frame.epilogue_instructions)
                if 'add sp, x29' not in epilogue_str:
                    warnings.append(
                        f"{name}: Pattern A prologue but epilogue may not match. "
                        "Expected 'add sp, x29, #16' pattern."
                    )
        
        return FrameAnalysis(
            functions=self.functions,
            issues=issues,
            warnings=warnings,
            target_isa=self.target_isa
        )


def compare_isas(objdump_x86: str, objdump_aarch64: str) -> Dict:
    """Compare stack frames across ISAs"""
    analyzer_x86 = StackFrameAnalyzer(objdump_x86, "x86_64")
    analysis_x86 = analyzer_x86.analyze()
    
    analyzer_aarch64 = StackFrameAnalyzer(objdump_aarch64, "aarch64")
    analysis_aarch64 = analyzer_aarch64.analyze()
    
    comparisons = []
    
    for name, frame_x86 in analysis_x86.functions.items():
        if name in analysis_aarch64.functions:
            frame_aarch64 = analysis_aarch64.functions[name]
            
            # Compare frame sizes
            size_diff = abs(frame_x86.frame_size - frame_aarch64.frame_size)
            if size_diff > 16:
                comparisons.append({
                    'function': name,
                    'issue': f"Frame size mismatch: x86={frame_x86.frame_size}, aarch64={frame_aarch64.frame_size}"
                })
            
            # Compare offsets
            x86_offsets = set([a.offset for a in frame_x86.stack_accesses])
            aarch64_offsets = set([a.offset for a in frame_aarch64.stack_accesses])
            
            if x86_offsets != aarch64_offsets:
                only_x86 = x86_offsets - aarch64_offsets
                only_aarch64 = aarch64_offsets - x86_offsets
                if only_aarch64:
                    comparisons.append({
                        'function': name,
                        'issue': f"AArch64 uses offsets not in x86: {only_aarch64}"
                    })
    
    return {
        'x86_analysis': analysis_x86,
        'aarch64_analysis': analysis_aarch64,
        'comparisons': comparisons
    }


def main():
    parser = argparse.ArgumentParser(
        description='Analyze stack frame layout across ISAs'
    )
    parser.add_argument('--object-file', help='Object file to analyze')
    parser.add_argument('--target', choices=['aarch64', 'x86_64'], default='aarch64',
                       help='Target architecture')
    parser.add_argument('--compare-x86', help='x86-64 object file for comparison')
    parser.add_argument('--compare-aarch64', help='AArch64 object file for comparison')
    parser.add_argument('--json', help='Output JSON analysis')
    parser.add_argument('--verbose', '-v', action='store_true')
    
    args = parser.parse_args()
    
    if args.compare_x86 and args.compare_aarch64:
        # Cross-ISA comparison
        try:
            result_x86 = subprocess.run(
                ['x86_64-linux-gnu-objdump', '-d', args.compare_x86],
                capture_output=True, text=True, check=True
            )
            result_aarch64 = subprocess.run(
                ['aarch64-linux-gnu-objdump', '-d', args.compare_aarch64],
                capture_output=True, text=True, check=True
            )
            
            comparison = compare_isas(result_x86.stdout, result_aarch64.stdout)
            
            print("=" * 80)
            print("CROSS-ISA STACK FRAME COMPARISON")
            print("=" * 80)
            
            if comparison['comparisons']:
                print("\nDiscrepancies found:")
                for comp in comparison['comparisons']:
                    print(f"  {comp['function']}: {comp['issue']}")
            else:
                print("\nNo discrepancies found between ISAs")
            
            if args.json:
                with open(args.json, 'w') as f:
                    json.dump(comparison, f, indent=2, default=str)
                print(f"\nJSON saved to: {args.json}")
            
        except subprocess.CalledProcessError as e:
            print(f"Error: {e}")
            sys.exit(1)
    
    elif args.object_file:
        # Single file analysis
        target = args.target
        if 'x86' in args.object_file:
            target = 'x86_64'
        
        try:
            result = subprocess.run(
                [f'{target}-linux-gnu-objdump', '-d', args.object_file],
                capture_output=True, text=True, check=True
            )
            
            analyzer = StackFrameAnalyzer(result.stdout, target)
            analysis = analyzer.analyze()
            
            print("=" * 80)
            print(f"STACK FRAME ANALYSIS: {args.object_file}")
            print("=" * 80)
            print(f"Target ISA: {analysis.target_isa}")
            print(f"Functions analyzed: {len(analysis.functions)}")
            print(f"Issues: {len(analysis.issues)}")
            print(f"Warnings: {len(analysis.warnings)}")
            print()
            
            for name, frame in analysis.functions.items():
                print(f"Function: {name}")
                print(f"  Frame size: {frame.frame_size} bytes")
                print(f"  Pattern: {frame.prologue_pattern}")
                if frame.stack_accesses:
                    offsets = sorted(set([a.offset for a in frame.stack_accesses]))
                    print(f"  Offsets: {offsets}")
                print()
            
            if analysis.issues:
                print("ISSUES:")
                for issue in analysis.issues:
                    print(f"  [ISSUE] {issue}")
                print()
            
            if analysis.warnings:
                print("WARNINGS:")
                for warning in analysis.warnings:
                    print(f"  [WARN] {warning}")
                print()
            
            if args.json:
                with open(args.json, 'w') as f:
                    json.dump({
                        'functions': {
                            name: {
                                'frame_size': frame.frame_size,
                                'pattern': frame.prologue_pattern,
                                'offsets': [a.offset for a in frame.stack_accesses]
                            }
                            for name, frame in analysis.functions.items()
                        },
                        'issues': analysis.issues,
                        'warnings': analysis.warnings
                    }, f, indent=2)
                print(f"JSON saved to: {args.json}")
            
        except subprocess.CalledProcessError as e:
            print(f"Error: {e}")
            sys.exit(1)
    
    else:
        parser.print_help()
        sys.exit(1)


if __name__ == '__main__':
    main()
