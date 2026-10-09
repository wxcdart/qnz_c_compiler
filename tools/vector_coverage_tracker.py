#!/usr/bin/env python3
"""
Vector Instruction Coverage Tracker for ZeroCC
Tracks SIMD instruction implementation and test coverage for x86-64 (SSE2), AArch64 (NEON), and RISC-V (RVV)

Usage:
    python3 vector_coverage_tracker.py --check-backend --isa x86_64
    python3 vector_coverage_tracker.py --check-backend --isa aarch64
    python3 vector_coverage_tracker.py --check-backend --isa riscv64
    python3 vector_coverage_tracker.py --analyze-tests <test_dir>
    python3 vector_coverage_tracker.py --report
"""

import sys
import os
import re
import argparse
from typing import Dict, List, Set, Optional
from dataclasses import dataclass, field
from pathlib import Path
from enum import Enum


class ISA(Enum):
    X86_64 = "x86_64"
    AARCH64 = "aarch64"
    RISCV64 = "riscv64"


@dataclass
class VectorInstruction:
    name: str
    category: str
    opcode: str
    implemented: bool = False
    test_coverage: Set[str] = field(default_factory=set)
    notes: str = ""


@dataclass
class CoverageReport:
    total_instructions: int = 0
    implemented_count: int = 0
    test_coverage_percent: float = 0.0
    by_category: Dict[str, Dict[str, int]] = field(default_factory=dict)
    missing_instructions: List[str] = field(default_factory=list)
    isa: ISA = ISA.X86_64


# x86-64 SSE2 vector instruction definitions
X86_64_VEC_INSTRUCTION_DEFS = {
    # Scalar Single-Precision (SS) Arithmetic
    'addss': ('arithmetic', '0xF20F58', 'Scalar single-precision add'),
    'subss': ('arithmetic', '0xF20F5C', 'Scalar single-precision subtract'),
    'mulss': ('arithmetic', '0xF20F59', 'Scalar single-precision multiply'),
    'divss': ('arithmetic', '0xF20F5E', 'Scalar single-precision divide'),
    
    # Scalar Double-Precision (SD) Arithmetic
    'addsd': ('arithmetic', '0xF30F58', 'Scalar double-precision add'),
    'subsd': ('arithmetic', '0xF30F5C', 'Scalar double-precision subtract'),
    'mulsd': ('arithmetic', '0xF30F59', 'Scalar double-precision multiply'),
    'divsd': ('arithmetic', '0xF30F5E', 'Scalar double-precision divide'),
    
    # Packed Single-Precision (PS) Arithmetic
    'addps': ('arithmetic', '0x0F58', 'Packed single-precision add'),
    'subps': ('arithmetic', '0x0F5C', 'Packed single-precision subtract'),
    'mulps': ('arithmetic', '0x0F59', 'Packed single-precision multiply'),
    'divps': ('arithmetic', '0x0F5E', 'Packed single-precision divide'),
    
    # Packed Double-Precision (PD) Arithmetic
    'addpd': ('arithmetic', '0x0F58', 'Packed double-precision add'),
    'subpd': ('arithmetic', '0x0F5C', 'Packed double-precision subtract'),
    'mulpd': ('arithmetic', '0x0F59', 'Packed double-precision multiply'),
    'divpd': ('arithmetic', '0x0F5E', 'Packed double-precision divide'),
    
    # Comparison
    'ucomiss': ('comparison', '0x0F2E', 'Unordered compare scalar single'),
    'ucomisd': ('comparison', '0x0F2E', 'Unordered compare scalar double'),
    'comiss': ('comparison', '0x0F2E', 'Compare scalar single'),
    'comisd': ('comparison', '0x0F2E', 'Compare scalar double'),
    'cmpps': ('comparison', '0x0FC2', 'Compare packed single'),
    'cmpsd': ('comparison', '0x0FC2', 'Compare scalar double'),
    
    # Logical/Bitwise
    'andps': ('logical', '0x0F54', 'Bitwise AND packed single'),
    'andpd': ('logical', '0x0F54', 'Bitwise AND packed double'),
    'andnps': ('logical', '0x0F55', 'Bitwise AND NOT packed single'),
    'andnpd': ('logical', '0x0F55', 'Bitwise AND NOT packed double'),
    'orps': ('logical', '0x0F56', 'Bitwise OR packed single'),
    'orpd': ('logical', '0x0F56', 'Bitwise OR packed double'),
    'xorps': ('logical', '0x0F57', 'Bitwise XOR packed single'),
    'xorpd': ('logical', '0x0F57', 'Bitwise XOR packed double'),
    
    # Data movement
    'movss': ('data_movement', '0xF20F10', 'Move scalar single'),
    'movsd': ('data_movement', '0xF30F10', 'Move scalar double'),
    'movaps': ('data_movement', '0x0F28', 'Move aligned packed single'),
    'movapd': ('data_movement', '0x0F28', 'Move aligned packed double'),
    'movups': ('data_movement', '0x0F10', 'Move unaligned packed single'),
    'movupd': ('data_movement', '0x0F10', 'Move unaligned packed double'),
    'movd': ('data_movement', '0x0F6E', 'Move doubleword'),
    'movq': ('data_movement', '0xF30FD6', 'Move quadword'),
    
    # Conversion
    'cvtsi2ss': ('conversion', '0xF20F2A', 'Convert int to scalar single'),
    'cvtsi2sd': ('conversion', '0xF30F2A', 'Convert int to scalar double'),
    'cvtss2si': ('conversion', '0xF20F2D', 'Convert scalar single to int'),
    'cvtsd2si': ('conversion', '0xF30F2D', 'Convert scalar double to int'),
    'cvtps2pd': ('conversion', '0x0F5A', 'Convert packed single to double'),
    'cvtpd2ps': ('conversion', '0x0F5A', 'Convert packed double to single'),
    
    # Shuffle
    'shufps': ('shuffle', '0x0FC6', 'Shuffle packed single'),
    'shufpd': ('shuffle', '0x0FC6', 'Shuffle packed double'),
}

# AArch64 NEON vector instruction definitions
AARCH64_VEC_INSTRUCTION_DEFS = {
    # Floating Point Arithmetic
    'fadd': ('arithmetic', '0x4E20', 'Floating point add'),
    'fsub': ('arithmetic', '0x4E20', 'Floating point subtract'),
    'fmul': ('arithmetic', '0x4E20', 'Floating point multiply'),
    'fdiv': ('arithmetic', '0x4E20', 'Floating point divide'),
    'fmax': ('arithmetic', '0x4E20', 'Floating point maximum'),
    'fmin': ('arithmetic', '0x4E20', 'Floating point minimum'),
    
    # Integer Arithmetic
    'add': ('arithmetic', '0x4E00', 'Vector add'),
    'sub': ('arithmetic', '0x4E00', 'Vector subtract'),
    'mul': ('arithmetic', '0x4E00', 'Vector multiply'),
    'smax': ('arithmetic', '0x4E00', 'Signed maximum'),
    'smin': ('arithmetic', '0x4E00', 'Signed minimum'),
    'umax': ('arithmetic', '0x4E00', 'Unsigned maximum'),
    'umin': ('arithmetic', '0x4E00', 'Unsigned minimum'),
    
    # Comparison
    'fcmp': ('comparison', '0x1E60', 'Floating point compare'),
    'cmeq': ('comparison', '0x0E20', 'Compare equal'),
    'cmge': ('comparison', '0x0E20', 'Compare greater or equal'),
    'cmgt': ('comparison', '0x0E20', 'Compare greater than'),
    
    # Data movement
    'mov': ('data_movement', '0x4E00', 'Move vector'),
    'dup': ('data_movement', '0x4E00', 'Duplicate element'),
    'ld1': ('data_movement', '0x4E00', 'Load single structure'),
    'st1': ('data_movement', '0x4E00', 'Store single structure'),
    
    # Conversion
    'fcvt': ('conversion', '0x1E60', 'Floating point convert'),
    'fcvtzs': ('conversion', '0x1E60', 'Convert to signed int'),
    'scvtf': ('conversion', '0x1E60', 'Signed int to float'),
    
    # Shuffle
    'ext': ('shuffle', '0x4E00', 'Extract'),
    'zip1': ('shuffle', '0x4E00', 'Zip first halves'),
    'zip2': ('shuffle', '0x4E00', 'Zip second halves'),
}

# RISC-V RVV vector instruction definitions
RISCV_VEC_INSTRUCTION_DEFS = {
    # Floating Point Arithmetic
    'vfadd': ('arithmetic', '0x43', 'Vector FP add'),
    'vfsub': ('arithmetic', '0x43', 'Vector FP subtract'),
    'vfmul': ('arithmetic', '0x43', 'Vector FP multiply'),
    'vfdiv': ('arithmetic', '0x43', 'Vector FP divide'),
    'vfmin': ('arithmetic', '0x43', 'Vector FP minimum'),
    'vfmax': ('arithmetic', '0x43', 'Vector FP maximum'),
    
    # Integer Arithmetic
    'vadd': ('arithmetic', '0x43', 'Vector add'),
    'vsub': ('arithmetic', '0x43', 'Vector subtract'),
    'vmul': ('arithmetic', '0x43', 'Vector multiply'),
    'vdiv': ('arithmetic', '0x43', 'Vector divide'),
    
    # Load/Store
    'vle32': ('data_movement', '0x07', 'Load element 32-bit'),
    'vle64': ('data_movement', '0x07', 'Load element 64-bit'),
    'vse32': ('data_movement', '0x07', 'Store element 32-bit'),
    'vse64': ('data_movement', '0x07', 'Store element 64-bit'),
    
    # Configuration
    'vsetvli': ('configuration', '0x43', 'Set vector length'),
    'vsetvl': ('configuration', '0x43', 'Set vector length'),
    
    # Conversion
    'vfcvt': ('conversion', '0x43', 'Vector FP convert'),
}


class VectorCoverageTracker:
    def __init__(self, isa: ISA = ISA.X86_64):
        self.isa = isa
        self.instructions: Dict[str, VectorInstruction] = {}
        self.backend_file: Optional[str] = None
        self.test_files: List[str] = []
        
        # Set instruction definitions based on ISA
        if isa == ISA.X86_64:
            self.instruction_defs = X86_64_VEC_INSTRUCTION_DEFS
            self.backend_prefix = 'x86'
        elif isa == ISA.AARCH64:
            self.instruction_defs = AARCH64_VEC_INSTRUCTION_DEFS
            self.backend_prefix = 'aarch64'
        else:  # RISCV64
            self.instruction_defs = RISCV_VEC_INSTRUCTION_DEFS
            self.backend_prefix = 'riscv'
        
        # Initialize all instructions
        for name, (category, opcode, desc) in self.instruction_defs.items():
            self.instructions[name] = VectorInstruction(
                name=name, category=category, opcode=opcode, notes=desc
            )
    
    def check_backend_implementation(self, backend_path: str) -> CoverageReport:
        """Check which vector instructions are implemented in backend"""
        self.backend_file = backend_path
        
        try:
            with open(backend_path, 'r') as f:
                backend_content = f.read()
        except IOError as e:
            print(f"Error reading backend file: {e}")
            return CoverageReport(isa=self.isa)
        
        # Check for each instruction implementation
        for instr_name in self.instructions:
            # Look for function implementations
            patterns = [
                rf'{self.backend_prefix}_emit_{instr_name}',
                rf'emit_{instr_name}',
                rf'\b{instr_name}\b',
            ]
            
            for pattern in patterns:
                if re.search(pattern, backend_content, re.IGNORECASE):
                    self.instructions[instr_name].implemented = True
                    break
        
        return self._generate_report()
    
    def analyze_tests(self, test_dir: str) -> CoverageReport:
        """Analyze test files for vector usage"""
        self.test_files = []
        test_path = Path(test_dir)
        
        # Find all test files
        for test_file in test_path.rglob('*_test.c'):
            self.test_files.append(str(test_file))
            self._analyze_test_file(str(test_file))
        
        for test_file in test_path.rglob('*.c'):
            if 'test' in str(test_file):
                self.test_files.append(str(test_file))
                self._analyze_test_file(str(test_file))
        
        return self._generate_report()
    
    def _analyze_test_file(self, filepath: str):
        """Analyze a single test file for vector-related code"""
        try:
            with open(filepath, 'r') as f:
                content = f.read()
        except IOError:
            return
        
        filename = os.path.basename(filepath)
        
        # Check for vector-related patterns
        vector_patterns = {
            'float': ['float', 'floats', 'floating', 'single'],
            'double': ['double', 'doubles'],
            'vector': ['vector', 'simd', 'sse', 'neon', 'rvv'],
            'array': ['array', 'arrays', 'loop'],
        }
        
        for category, patterns in vector_patterns.items():
            for pattern in patterns:
                if pattern.lower() in content.lower():
                    # Mark relevant instructions as tested
                    for instr_name, instr in self.instructions.items():
                        if instr.category == category or category in instr.name:
                            instr.test_coverage.add(filename)
                    break
    
    def _generate_report(self) -> CoverageReport:
        """Generate coverage report"""
        report = CoverageReport(isa=self.isa)
        report.total_instructions = len(self.instructions)
        report.implemented_count = sum(
            1 for instr in self.instructions.values() if instr.implemented
        )
        
        # Calculate by category
        categories = set(instr.category for instr in self.instructions.values())
        for category in categories:
            cat_instructions = [
                instr for instr in self.instructions.values() 
                if instr.category == category
            ]
            cat_implemented = sum(1 for instr in cat_instructions if instr.implemented)
            report.by_category[category] = {
                'total': len(cat_instructions),
                'implemented': cat_implemented,
            }
        
        # Find missing instructions
        report.missing_instructions = [
            instr.name for instr in self.instructions.values()
            if not instr.implemented
        ]
        
        # Calculate coverage
        if report.total_instructions > 0:
            tested = sum(
                1 for instr in self.instructions.values()
                if len(instr.test_coverage) > 0
            )
            report.test_coverage_percent = (tested / report.total_instructions) * 100
        
        return report
    
    def generate_report_text(self, report: CoverageReport) -> str:
        """Generate text report"""
        isa_name = self.isa.value.upper()
        lines = []
        lines.append("=" * 70)
        lines.append(f"Vector Instruction Coverage Report ({isa_name})")
        lines.append("=" * 70)
        
        lines.append(f"\nTotal Vector Instructions: {report.total_instructions}")
        if report.total_instructions > 0:
            pct = report.implemented_count/report.total_instructions*100
            lines.append(f"Implemented: {report.implemented_count} ({pct:.1f}%)")
        lines.append(f"Test Coverage: {report.test_coverage_percent:.1f}%")
        
        if report.by_category:
            lines.append("\nBy Category:")
            lines.append("-" * 70)
            for category, counts in sorted(report.by_category.items()):
                pct = counts['implemented'] / counts['total'] * 100 if counts['total'] > 0 else 0
                lines.append(f"  {category:20s}: {counts['implemented']:3d}/{counts['total']:3d} ({pct:5.1f}%)")
        
        if report.missing_instructions:
            lines.append(f"\nMissing Implementations ({len(report.missing_instructions)}):")
            lines.append("-" * 70)
            for instr_name in sorted(report.missing_instructions)[:20]:
                instr = self.instructions[instr_name]
                lines.append(f"  - {instr_name:15s} ({instr.category}): {instr.notes}")
            if len(report.missing_instructions) > 20:
                lines.append(f"  ... and {len(report.missing_instructions) - 20} more")
        
        lines.append("=" * 70)
        return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(
        description='Track vector instruction coverage in ZeroCC (x86-64 SSE2, AArch64 NEON, RISC-V RVV)'
    )
    parser.add_argument('--isa', choices=['x86_64', 'aarch64', 'riscv64'], default='x86_64',
                        help='Target ISA (default: x86_64)')
    parser.add_argument('--check-backend', metavar='BACKEND_FILE',
                       help='Check backend implementation')
    parser.add_argument('--analyze-tests', metavar='TEST_DIR',
                       help='Analyze test directory')
    parser.add_argument('--report', action='store_true',
                       help='Generate report')
    parser.add_argument('--output', '-o', metavar='FILE',
                       help='Output file for report')
    
    args = parser.parse_args()
    
    isa = ISA(args.isa)
    tracker = VectorCoverageTracker(isa=isa)
    report = CoverageReport(isa=isa)
    
    if args.check_backend:
        report = tracker.check_backend_implementation(args.check_backend)
    elif args.analyze_tests:
        report = tracker.analyze_tests(args.analyze_tests)
    else:
        parser.print_help()
        sys.exit(1)
    
    report_text = tracker.generate_report_text(report)
    
    if args.output:
        with open(args.output, 'w') as f:
            f.write(report_text)
        print(f"Report written to {args.output}")
    else:
        print(report_text)


if __name__ == '__main__':
    main()
