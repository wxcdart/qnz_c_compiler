#!/usr/bin/env python3
"""
SIMD IR Validator for ZeroCC
Validates SIMD IR opcodes and instruction format

Usage:
    python3 simd_ir_validator.py <ir_dump_file>
    python3 simd_ir_validator.py --check-all <ir_dump_file>
    python3 simd_ir_validator.py --verbose <ir_dump_file>
"""

import sys
import re
import argparse
from typing import Dict, List, Set, Optional, Tuple
from dataclasses import dataclass, field


@dataclass
class SIMDIssue:
    """Represents a SIMD IR validation issue"""
    severity: str  # 'ERROR', 'WARNING', 'INFO'
    line_num: int
    message: str
    instruction: str = ""


@dataclass
class SIMDIRStats:
    """Statistics about SIMD IR instructions"""
    total_instructions: int = 0
    simd_instructions: int = 0
    by_opcode: Dict[str, int] = field(default_factory=dict)
    by_category: Dict[str, int] = field(default_factory=dict)
    issues: List[SIMDIssue] = field(default_factory=list)


# SIMD IR opcode definitions
SIMD_IR_OPCODES = {
    # Vector arithmetic
    'OP_VADD': ('arithmetic', 'vector add'),
    'OP_VSUB': ('arithmetic', 'vector subtract'),
    'OP_VMUL': ('arithmetic', 'vector multiply'),
    'OP_VDIV': ('arithmetic', 'vector divide'),
    
    # Vector comparison
    'OP_VCMP_EQ': ('comparison', 'vector equal'),
    'OP_VCMP_LT': ('comparison', 'vector less than'),
    'OP_VCMP_LE': ('comparison', 'vector less or equal'),
    'OP_VCMP_GT': ('comparison', 'vector greater than'),
    'OP_VCMP_GE': ('comparison', 'vector greater or equal'),
    
    # Vector data movement
    'OP_VLOAD': ('data_movement', 'vector load'),
    'OP_VSTORE': ('data_movement', 'vector store'),
    'OP_VMOV': ('data_movement', 'vector move'),
    
    # Vector conversion
    'OP_VCVT_SI2SS': ('conversion', 'scalar int to single'),
    'OP_VCVT_SS2SI': ('conversion', 'scalar single to int'),
    'OP_VCVT_PS2PD': ('conversion', 'packed single to double'),
    'OP_VCVT_PD2PS': ('conversion', 'packed double to single'),
    
    # Vector shuffle
    'OP_VSHUF': ('shuffle', 'vector shuffle'),
    'OP_VUNPACK_LO': ('shuffle', 'unpack low'),
    'OP_VUNPACK_HI': ('shuffle', 'unpack high'),
    
    # Vector logical
    'OP_VAND': ('logical', 'vector AND'),
    'OP_VOR': ('logical', 'vector OR'),
    'OP_VXOR': ('logical', 'vector XOR'),
    'OP_VNOT': ('logical', 'vector NOT'),
    
    # Horizontal operations
    'OP_VHADD': ('horizontal', 'horizontal add'),
    'OP_VHSUB': ('horizontal', 'horizontal subtract'),
}


class SIMDIRValidator:
    def __init__(self):
        self.issues: List[SIMDIssue] = []
        self.stats = SIMDIRStats()
        self.current_function: str = ""
    
    def validate_file(self, filepath: str) -> SIMDIRStats:
        """Validate SIMD IR in file"""
        try:
            with open(filepath, 'r') as f:
                content = f.read()
        except IOError as e:
            self.issues.append(SIMDIssue(
                'ERROR', 0, f"Cannot read file: {filepath} - {e}"
            ))
            return self.stats
        
        self.stats = SIMDIRStats()
        self.issues = []
        
        lines = content.split('\n')
        for line_num, line in enumerate(lines, 1):
            self._validate_line(line, line_num, lines)
        
        self.stats.issues = self.issues
        return self.stats
    
    def _validate_line(self, line: str, line_num: int, all_lines: List[str]):
        """Validate a single IR line"""
        line = line.strip()
        
        if not line:
            return
        
        self.stats.total_instructions += 1
        
        # Check for SIMD IR opcodes
        for opcode, (category, desc) in SIMD_IR_OPCODES.items():
            if opcode in line:
                self.stats.simd_instructions += 1
                self.stats.by_opcode[opcode] = self.stats.by_opcode.get(opcode, 0) + 1
                self.stats.by_category[category] = self.stats.by_category.get(category, 0) + 1
                
                # Validate instruction format
                self._validate_instruction_format(opcode, line, line_num)
                return
        
        # Check for scalar floating point operations (potential SIMD candidates)
        if any(fp_op in line for fp_op in ['OP_FADD', 'OP_FSUB', 'OP_FMUL', 'OP_FDIV']):
            # These could potentially be vectorized
            pass
    
    def _validate_instruction_format(self, opcode: str, line: str, line_num: int):
        """Validate SIMD IR instruction format"""
        # Check for required operands
        if opcode.startswith('OP_VADD') or opcode.startswith('OP_VSUB') or \
           opcode.startswith('OP_VMUL') or opcode.startswith('OP_VDIV'):
            # Arithmetic operations need 3 operands: dst, src1, src2
            if 'r' not in line and '[' not in line:
                self.issues.append(SIMDIssue(
                    'WARNING', line_num,
                    f"{opcode} may be missing operands",
                    line
                ))
        
        # Check for valid register references
        xmm_pattern = re.compile(r'xmm(\d+)|r(\d+)')
        matches = xmm_pattern.findall(line)
        
        for match in matches:
            if match[0]:  # XMM register
                reg_num = int(match[0])
                if reg_num > 15:
                    self.issues.append(SIMDIssue(
                        'ERROR', line_num,
                        f"Invalid XMM register: xmm{reg_num} (must be 0-15)",
                        line
                    ))
            elif match[1]:  # General register
                reg_num = int(match[1])
                if reg_num > 255:
                    self.issues.append(SIMDIssue(
                        'WARNING', line_num,
                        f"Unusual register number: r{reg_num}",
                        line
                    ))
    
    def generate_report(self) -> str:
        """Generate validation report"""
        lines = []
        lines.append("=" * 70)
        lines.append("SIMD IR Validation Report")
        lines.append("=" * 70)
        
        lines.append(f"\nTotal Instructions: {self.stats.total_instructions}")
        lines.append(f"SIMD Instructions: {self.stats.simd_instructions}")
        
        if self.stats.total_instructions > 0:
            pct = self.stats.simd_instructions / self.stats.total_instructions * 100
            lines.append(f"SIMD Coverage: {pct:.1f}%")
        
        if self.stats.by_opcode:
            lines.append("\nSIMD Opcodes Used:")
            lines.append("-" * 70)
            for opcode, count in sorted(self.stats.by_opcode.items()):
                category = SIMD_IR_OPCODES.get(opcode, ('unknown', '?'))[0]
                lines.append(f"  {opcode:20s}: {count:3d} ({category})")
        
        if self.stats.by_category:
            lines.append("\nBy Category:")
            lines.append("-" * 70)
            for category, count in sorted(self.stats.by_category.items()):
                lines.append(f"  {category:20s}: {count:3d}")
        
        if self.issues:
            lines.append(f"\nIssues Found ({len(self.issues)}):")
            lines.append("-" * 70)
            for issue in self.issues:
                lines.append(f"[{issue.severity}] Line {issue.line_num}: {issue.message}")
                if issue.instruction:
                    lines.append(f"         {issue.instruction}")
        else:
            lines.append("\nNo issues found!")
        
        lines.append("=" * 70)
        return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(
        description='Validate SIMD IR opcodes in ZeroCC'
    )
    parser.add_argument('file', nargs='?', help='IR dump file to validate')
    parser.add_argument('--check-all', action='store_true', help='Check all IR (not just SIMD)')
    parser.add_argument('--verbose', '-v', action='store_true', help='Verbose output')
    
    args = parser.parse_args()
    
    if not args.file:
        parser.print_help()
        sys.exit(1)
    
    validator = SIMDIRValidator()
    validator.validate_file(args.file)
    
    print(validator.generate_report())
    
    # Exit with error if there are errors
    errors = [i for i in validator.issues if i.severity == 'ERROR']
    sys.exit(1 if errors else 0)


if __name__ == '__main__':
    main()
