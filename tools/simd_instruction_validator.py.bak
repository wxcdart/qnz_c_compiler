#!/usr/bin/env python3
"""
SIMD Instruction Validator for ZeroCC
Validates SSE2/SIMD instruction generation for x86-64 backend

Usage:
    python3 simd_instruction_validator.py <assembly_file>
    python3 simd_instruction_validator.py --check-ir <ir_dump>
"""

import sys
import re
import argparse
from typing import List, Dict, Tuple, Set

# SSE2 instruction opcodes and patterns
SSE2_INSTRUCTIONS = {
    # Arithmetic
    'addps': 0x0F58, 'addss': 0x0F58, 'subps': 0x0F5C, 'subss': 0x0F5C,
    'mulps': 0x0F59, 'mulss': 0x0F59, 'divps': 0x0F5E, 'divss': 0x0F5E,
    
    # Comparison
    'cmpps': 0x0FC2, 'cmpss': 0x0FC2,
    
    # Logical
    'andps': 0x0F54, 'andnps': 0x0F55, 'orps': 0x0F56, 'xorps': 0x0F57,
    
    # Data movement
    'movaps': 0x0F28, 'movups': 0x0F10, 'movss': 0x0F10, 'movsd': 0x0F10,
    'movdqa': 0x0F6F, 'movdqu': 0x0F6F, 'movdq2q': 0x0FD6, 'movq2dq': 0x0FE6,
    
    # Conversion
    'cvtps2pd': 0x0F5A, 'cvtpd2ps': 0x0F5A, 'cvtdq2ps': 0x0F5B,
    'cvtsi2ss': 0x0F2A, 'cvtss2si': 0x0F2A, 'cvtsi2sd': 0x0F2A, 'cvttsd2si': 0x0F2C,
    
    # Shuffle
    'shufps': 0x0FC6,
    
    # Unpack/pack
    'unpcklps': 0x0F14, 'unpckhps': 0x0F15,
}

# XMM register pattern
XMM_REG_PATTERN = re.compile(r'xmm([0-9]+)')

# SIMD instruction patterns in assembly
SIMD_ASM_PATTERNS = {
    'movaps': re.compile(r'\bmovaps\s+'),
    'movups': re.compile(r'\bmovups\s+'),
    'movss': re.compile(r'\bmovss\s+'),
    'movsd': re.compile(r'\bmovsd\s+'),
    'addss': re.compile(r'\baddss\s+'),
    'addsd': re.compile(r'\baddsd\s+'),
    'subss': re.compile(r'\bsubss\s+'),
    'subsd': re.compile(r'\bsubsd\s+'),
    'mulss': re.compile(r'\bmulss\s+'),
    'mulsd': re.compile(r'\bmulsd\s+'),
    'divss': re.compile(r'\bdivss\s+'),
    'divsd': re.compile(r'\bdivsd\s+'),
    'cvtsi2ss': re.compile(r'\bcvtsi2ss\s+'),
    'cvtsi2sd': re.compile(r'\bcvtsi2sd\s+'),
    'cvtss2si': re.compile(r'\bcvtss2si\s+'),
    'cvttsd2si': re.compile(r'\bcvttsd2si\s+'),
    'ucomiss': re.compile(r'\bucomiss\s+'),
    'ucomisd': re.compile(r'\bucomisd\s+'),
}


class SIMDValidationIssue:
    def __init__(self, severity: str, line_num: int, message: str, instruction: str = ""):
        self.severity = severity  # 'ERROR', 'WARNING', 'INFO'
        self.line_num = line_num
        self.message = message
        self.instruction = instruction
    
    def __str__(self):
        return f"[{self.severity}] Line {self.line_num}: {self.message}"


class SIMDInstructionValidator:
    def __init__(self):
        self.issues: List[SIMDValidationIssue] = []
        self.xmm_registers: Set[int] = set()
        self.simd_instructions_found: Dict[str, int] = {}
    
    def validate_xmm_register(self, reg_str: str) -> bool:
        """Validate XMM register format and range"""
        match = XMM_REG_PATTERN.search(reg_str)
        if not match:
            return False
        
        reg_num = int(match.group(1))
        if reg_num > 15:
            self.issues.append(SIMDValidationIssue(
                'ERROR', 0, f"Invalid XMM register: {reg_str} (must be xmm0-xmm15)"
            ))
            return False
        
        self.xmm_registers.add(reg_num)
        return True
    
    def validate_instruction(self, line: str, line_num: int) -> bool:
        """Validate a single assembly instruction"""
        line = line.strip()
        if not line or line.startswith('#') or line.startswith('.'):
            return True
        
        # Check for SIMD instructions
        for instr_name, pattern in SIMD_ASM_PATTERNS.items():
            if pattern.search(line):
                self.simd_instructions_found[instr_name] = \
                    self.simd_instructions_found.get(instr_name, 0) + 1
                
                # Validate XMM registers in instruction
                xmm_matches = XMM_REG_PATTERN.findall(line)
                for xmm_reg in xmm_matches:
                    self.validate_xmm_register(f"xmm{xmm_reg}")
                
                return True
        
        return True
    
    def validate_file(self, filepath: str) -> bool:
        """Validate SIMD instructions in assembly file"""
        try:
            with open(filepath, 'r') as f:
                lines = f.readlines()
        except IOError as e:
            self.issues.append(SIMDValidationIssue(
                'ERROR', 0, f"Cannot read file: {filepath} - {e}"
            ))
            return False
        
        for line_num, line in enumerate(lines, 1):
            self.validate_instruction(line, line_num)
        
        return len([i for i in self.issues if i.severity == 'ERROR']) == 0
    
    def validate_ir(self, ir_content: str) -> bool:
        """Validate SIMD-related IR instructions"""
        lines = ir_content.split('\n')
        
        for line_num, line in enumerate(lines, 1):
            # Check for SIMD IR opcodes (future implementation)
            if 'OP_VADD' in line or 'OP_VMUL' in line or 'OP_VLOAD' in line:
                self.issues.append(SIMDValidationIssue(
                    'INFO', line_num, f"Found SIMD IR instruction: {line.strip()}"
                ))
        
        return True
    
    def generate_report(self) -> str:
        """Generate validation report"""
        report = []
        report.append("=" * 60)
        report.append("SIMD Instruction Validation Report")
        report.append("=" * 60)
        
        if self.issues:
            report.append(f"\nIssues Found: {len(self.issues)}")
            report.append("-" * 60)
            for issue in self.issues:
                report.append(str(issue))
        else:
            report.append("\nNo issues found!")
        
        if self.simd_instructions_found:
            report.append(f"\nSIMD Instructions Found: {len(self.simd_instructions_found)}")
            report.append("-" * 60)
            for instr, count in sorted(self.simd_instructions_found.items()):
                report.append(f"  {instr}: {count}")
        
        if self.xmm_registers:
            report.append(f"\nXMM Registers Used: {sorted(self.xmm_registers)}")
        
        report.append("=" * 60)
        return '\n'.join(report)


def main():
    parser = argparse.ArgumentParser(
        description='Validate SIMD instruction generation for ZeroCC'
    )
    parser.add_argument('file', nargs='?', help='Assembly file to validate')
    parser.add_argument('--check-ir', metavar='IR_FILE', help='Validate IR dump')
    parser.add_argument('--verbose', '-v', action='store_true', help='Verbose output')
    
    args = parser.parse_args()
    
    validator = SIMDInstructionValidator()
    
    if args.check_ir:
        try:
            with open(args.check_ir, 'r') as f:
                ir_content = f.read()
            success = validator.validate_ir(ir_content)
        except IOError as e:
            print(f"Error reading IR file: {e}")
            sys.exit(1)
    elif args.file:
        success = validator.validate_file(args.file)
    else:
        parser.print_help()
        sys.exit(1)
    
    print(validator.generate_report())
    
    sys.exit(0 if success else 1)


if __name__ == '__main__':
    main()
