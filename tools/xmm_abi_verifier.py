#!/usr/bin/env python3
"""
XMM ABI Verifier for ZeroCC
Verifies XMM register calling convention compliance

Usage:
    python3 xmm_abi_verifier.py --check-callee-save <assembly_file>
    python3 xmm_abi_verifier.py --check-params <assembly_file>
    python3 xmm_abi_verifier.py --check-returns <assembly_file>
    python3 xmm_abi_verifier.py --verify-all <assembly_file>
"""

import sys
import re
import argparse
from typing import Dict, List, Set, Optional, Tuple
from dataclasses import dataclass


@dataclass
class ABIIssue:
    """Represents an ABI violation"""
    severity: str  # 'ERROR', 'WARNING', 'INFO'
    line_num: int
    message: str
    instruction: str = ""


# System V AMD64 ABI XMM register conventions
# Caller-saved (volatile): xmm0-xmm7
# Callee-saved (non-volatile): xmm6-xmm7 (actually, in System V ABI, all XMM are caller-saved)
# Return value: xmm0 (for scalar float/double)
# Parameters: xmm0-xmm7 (for first 8 float/double parameters)

XMM_CALLER_SAVED = set(range(8))  # xmm0-xmm7
XMM_RETURN_REG = 0  # xmm0
XMM_PARAM_REGS = list(range(8))  # xmm0-xmm7 for first 8 float/double params


class XMMABIVerifier:
    def __init__(self):
        self.issues: List[ABIIssue] = []
        self.current_function: str = ""
        self.function_start: int = 0
        self.function_end: int = 0
        self.params: List[int] = []  # XMM registers used for parameters
        self.returns: List[int] = []  # XMM registers used for returns
        self.callee_saved_uses: Dict[int, List[int]] = {}  # reg -> [save_line, restore_line]
    
    def verify_file(self, filepath: str) -> List[ABIIssue]:
        """Verify XMM ABI compliance in assembly file"""
        try:
            with open(filepath, 'r') as f:
                lines = f.readlines()
        except IOError as e:
            self.issues.append(ABIIssue(
                'ERROR', 0, f"Cannot read file: {filepath} - {e}"
            ))
            return self.issues
        
        self.issues = []
        self._analyze_file(lines)
        
        return self.issues
    
    def _analyze_file(self, lines: List[str]):
        """Analyze assembly file for ABI compliance"""
        in_function = False
        function_lines = []
        
        for line_num, line in enumerate(lines, 1):
            stripped = line.strip()
            
            # Detect function start
            if stripped.endswith(':') and not stripped.startswith('.'):
                if in_function and function_lines:
                    self._verify_function(self.current_function, function_lines)
                
                self.current_function = stripped[:-1]
                self.function_start = line_num
                in_function = True
                function_lines = [(line_num, stripped)]
            
            elif in_function:
                function_lines.append((line_num, stripped))
                
                # Detect function end (ret or end of file)
                if stripped.startswith('ret'):
                    self.function_end = line_num
                    self._verify_function(self.current_function, function_lines)
                    in_function = False
                    function_lines = []
        
        # Handle last function if no ret
        if in_function and function_lines:
            self._verify_function(self.current_function, function_lines)
    
    def _verify_function(self, func_name: str, lines: List[Tuple[int, str]]):
        """Verify ABI compliance for a single function"""
        self.params = []
        self.returns = []
        self.callee_saved_uses = {}
        
        xmm_pattern = re.compile(r'xmm(\d+)')
        mov_pattern = re.compile(r'\bmov(?:[a-z]*)\s+')
        
        # Analyze function body
        for line_num, line in lines:
            # Check for parameter usage (xmm0-xmm7 in function prologue)
            if line_num <= self.function_start + 5:
                matches = xmm_pattern.findall(line)
                for match in matches:
                    reg = int(match)
                    if reg in XMM_PARAM_REGS:
                        if reg not in self.params:
                            self.params.append(reg)
            
            # Check for return value
            if 'ret' in line or line.startswith('ret'):
                # Check if xmm0 was used
                for prev_line_num, prev_line in lines:
                    if xmm_pattern.search(prev_line):
                        matches = xmm_pattern.findall(prev_line)
                        for match in matches:
                            reg = int(match)
                            if reg == XMM_RETURN_REG:
                                self.returns.append(reg)
                                break
            
            # Check for callee-saved register usage (xmm6-xmm7)
            matches = xmm_pattern.findall(line)
            for match in matches:
                reg = int(match)
                if reg not in self.callee_saved_uses:
                    self.callee_saved_uses[reg] = []
                self.callee_saved_uses[reg].append(line_num)
        
        # Verify parameter passing
        self._verify_parameters(func_name, lines)
        
        # Verify return value
        self._verify_return(func_name, lines)
        
        # Verify callee-saved registers (if any)
        self._verify_callee_saved(func_name, lines)
    
    def _verify_parameters(self, func_name: str, lines: List[Tuple[int, str]]):
        """Verify parameter passing convention"""
        # Check if float/double parameters are in XMM registers
        # This is a simplified check - full check would need to analyze the function signature
        
        # Look for moves from stack to XMM (indicating parameters might be on stack)
        xmm_pattern = re.compile(r'xmm(\d+)')
        stack_pattern = re.compile(r'\[rbp-')
        
        for line_num, line in lines:
            if stack_pattern.search(line) and xmm_pattern.search(line):
                # Parameter might be loaded from stack to XMM
                # This could be correct or incorrect depending on calling convention
                self.issues.append(ABIIssue(
                    'INFO', line_num,
                    f"Function {func_name}: Parameter loaded from stack to XMM",
                    line
                ))
    
    def _verify_return(self, func_name: str, lines: List[Tuple[int, str]]):
        """Verify return value convention"""
        # Float/double return values should be in xmm0
        
        # Check if function returns float/double but doesn't use xmm0
        has_float_return = False
        uses_xmm0 = False
        
        for line_num, line in lines:
            # Check for float operations
            if any(op in line for op in ['movss', 'movsd', 'addss', 'mulss', 'divss']):
                has_float_return = True
            
            # Check for xmm0 usage
            if 'xmm0' in line:
                uses_xmm0 = True
        
        if has_float_return and not uses_xmm0:
            self.issues.append(ABIIssue(
                'WARNING', self.function_start,
                f"Function {func_name}: May return float but doesn't use xmm0"
            ))
    
    def _verify_callee_saved(self, func_name: str, lines: List[Tuple[int, str]]):
        """Verify callee-saved register handling"""
        # In System V AMD64 ABI, xmm6-xmm7 are callee-saved
        # But in practice, most compilers treat all XMM as caller-saved
        
        for reg, line_nums in self.callee_saved_uses.items():
            if reg >= 6:  # xmm6-xmm7
                # Check if register is saved and restored
                save_found = False
                restore_found = False
                
                for line_num, line in lines:
                    if reg == 6:
                        if 'push' in line and 'xmm6' in line:
                            save_found = True
                        if 'pop' in line and 'xmm6' in line:
                            restore_found = True
                    elif reg == 7:
                        if 'push' in line and 'xmm7' in line:
                            save_found = True
                        if 'pop' in line and 'xmm7' in line:
                            restore_found = True
                
                # Note: Most compilers don't save XMM6-XMM7
                # This is a warning, not an error
                if line_nums and not save_found:
                    self.issues.append(ABIIssue(
                        'INFO', line_nums[0],
                        f"Function {func_name}: Uses xmm{reg} (callee-saved) without explicit save"
                    ))
    
    def generate_report(self) -> str:
        """Generate verification report"""
        lines = []
        lines.append("=" * 70)
        lines.append("XMM ABI Verification Report")
        lines.append("=" * 70)
        
        if self.issues:
            lines.append(f"\nIssues Found: {len(self.issues)}")
            lines.append("-" * 70)
            
            errors = [i for i in self.issues if i.severity == 'ERROR']
            warnings = [i for i in self.issues if i.severity == 'WARNING']
            infos = [i for i in self.issues if i.severity == 'INFO']
            
            lines.append(f"  Errors:   {len(errors)}")
            lines.append(f"  Warnings: {len(warnings)}")
            lines.append(f"  Info:     {len(infos)}")
            
            lines.append("\nDetailed Issues:")
            for issue in self.issues:
                lines.append(f"[{issue.severity}] Line {issue.line_num}: {issue.message}")
                if issue.instruction:
                    lines.append(f"         {issue.instruction}")
        else:
            lines.append("\nNo ABI issues found!")
        
        lines.append("\nABI Conventions:")
        lines.append("-" * 70)
        lines.append("  Caller-saved: xmm0-xmm7")
        lines.append("  Callee-saved: (none for XMM in practice)")
        lines.append("  Return:       xmm0 (for float/double)")
        lines.append("  Parameters:   xmm0-xmm7 (first 8 float/double)")
        
        lines.append("=" * 70)
        return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(
        description='Verify XMM register ABI compliance in ZeroCC'
    )
    parser.add_argument('file', nargs='?', help='Assembly file to verify')
    parser.add_argument('--check-callee-save', action='store_true',
                       help='Check callee-saved registers')
    parser.add_argument('--check-params', action='store_true',
                       help='Check parameter passing')
    parser.add_argument('--check-returns', action='store_true',
                       help='Check return values')
    parser.add_argument('--verify-all', action='store_true',
                       help='Verify all ABI aspects')
    
    args = parser.parse_args()
    
    if not args.file:
        parser.print_help()
        sys.exit(1)
    
    verifier = XMMABIVerifier()
    verifier.verify_file(args.file)
    
    print(verifier.generate_report())
    
    # Exit with error if there are errors
    errors = [i for i in verifier.issues if i.severity == 'ERROR']
    sys.exit(1 if errors else 0)


if __name__ == '__main__':
    main()
