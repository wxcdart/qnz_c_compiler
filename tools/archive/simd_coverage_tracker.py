#!/usr/bin/env python3
"""
SIMD Coverage Tracker for ZeroCC
Tracks SIMD instruction implementation and test coverage

Usage:
    python3 simd_coverage_tracker.py --check-backend
    python3 simd_coverage_tracker.py --analyze-tests <test_dir>
    python3 simd_coverage_tracker.py --report
"""

import sys
import os
import re
import argparse
from typing import Dict, List, Set, Optional
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class SIMDInstruction:
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


# SSE2/SIMD instruction categories
SIMD_INSTRUCTION_DEFS = {
    # Arithmetic operations
    'addss': ('arithmetic', '0xF20F58', 'Scalar single-precision add'),
    'addsd': ('arithmetic', '0xF30F58', 'Scalar double-precision add'),
    'subss': ('arithmetic', '0xF20F5C', 'Scalar single-precision subtract'),
    'subsd': ('arithmetic', '0xF30F5C', 'Scalar double-precision subtract'),
    'mulss': ('arithmetic', '0xF20F59', 'Scalar single-precision multiply'),
    'mulsd': ('arithmetic', '0xF30F59', 'Scalar double-precision multiply'),
    'divss': ('arithmetic', '0xF20F5E', 'Scalar single-precision divide'),
    'divsd': ('arithmetic', '0xF30F5E', 'Scalar double-precision divide'),
    
    # Comparison operations
    'ucomiss': ('comparison', '0x0F2E', 'Unordered compare scalar single'),
    'ucomisd': ('comparison', '0x0F2E', 'Unordered compare scalar double'),
    'comiss': ('comparison', '0x0F2E', 'Compare scalar single'),
    'comisd': ('comparison', '0x0F2E', 'Compare scalar double'),
    
    # Data movement
    'movss': ('data_movement', '0xF20F10', 'Move scalar single'),
    'movsd': ('data_movement', '0xF30F10', 'Move scalar double'),
    'movaps': ('data_movement', '0x0F28', 'Move aligned packed single'),
    'movapd': ('data_movement', '0x0F28', 'Move aligned packed double'),
    'movups': ('data_movement', '0x0F10', 'Move unaligned packed single'),
    'movupd': ('data_movement', '0x0F10', 'Move unaligned packed double'),
    'movd': ('data_movement', '0x0F6E', 'Move doubleword to/from XMM'),
    'movq': ('data_movement', '0xF30FD6', 'Move quadword to/from XMM'),
    
    # Conversion
    'cvtsi2ss': ('conversion', '0xF20F2A', 'Convert int to scalar single'),
    'cvtsi2sd': ('conversion', '0xF30F2A', 'Convert int to scalar double'),
    'cvtss2si': ('conversion', '0xF20F2D', 'Convert scalar single to int'),
    'cvtsd2si': ('conversion', '0xF30F2D', 'Convert scalar double to int'),
    'cvtss2sd': ('conversion', '0xF30F5A', 'Convert scalar single to double'),
    'cvtsd2ss': ('conversion', '0xF20F5A', 'Convert scalar double to single'),
    'cvttps2dq': ('conversion', '0xF30F5B', 'Convert with truncation packed'),
    
    # Logical operations
    'andps': ('logical', '0x0F54', 'Bitwise AND packed single'),
    'andpd': ('logical', '0x0F54', 'Bitwise AND packed double'),
    'orps': ('logical', '0x0F56', 'Bitwise OR packed single'),
    'orpd': ('logical', '0x0F56', 'Bitwise OR packed double'),
    'xorps': ('logical', '0x0F57', 'Bitwise XOR packed single'),
    'xorpd': ('logical', '0x0F57', 'Bitwise XOR packed double'),
    'notps': ('logical', '0x0F55', 'Bitwise NOT packed single'),
    'notpd': ('logical', '0x0F55', 'Bitwise NOT packed double'),
    
    # Shuffle and unpack
    'shufps': ('shuffle', '0x0FC6', 'Shuffle packed single'),
    'shufpd': ('shuffle', '0x0FC6', 'Shuffle packed double'),
    'unpcklps': ('shuffle', '0x0F14', 'Unpack low packed single'),
    'unpckhps': ('shuffle', '0x0F15', 'Unpack high packed single'),
    'unpcklpd': ('shuffle', '0x0F14', 'Unpack low packed double'),
    'unpckhpds': ('shuffle', '0x0F15', 'Unpack high packed double'),
    
    # Horizontal operations (SSE3)
    'haddps': ('horizontal', '0x0F7C', 'Horizontal add packed single'),
    'haddpd': ('horizontal', '0x0F7C', 'Horizontal add packed double'),
    'hsubps': ('horizontal', '0x0F7D', 'Horizontal subtract packed single'),
    'hsubpd': ('horizontal', '0x0F7D', 'Horizontal subtract packed double'),
}


class SIMDCoverageTracker:
    def __init__(self):
        self.instructions: Dict[str, SIMDInstruction] = {}
        self.backend_file: Optional[str] = None
        self.test_files: List[str] = []
        
        # Initialize all instructions
        for name, (category, opcode, desc) in SIMD_INSTRUCTION_DEFS.items():
            self.instructions[name] = SIMDInstruction(
                name=name, category=category, opcode=opcode, notes=desc
            )
    
    def check_backend_implementation(self, backend_path: str) -> CoverageReport:
        """Check which SIMD instructions are implemented in backend"""
        self.backend_file = backend_path
        
        try:
            with open(backend_path, 'r') as f:
                backend_content = f.read()
        except IOError as e:
            print(f"Error reading backend file: {e}")
            return CoverageReport()
        
        # Check for each instruction implementation
        for instr_name in self.instructions:
            # Look for function implementations
            patterns = [
                rf'x86_emit_{instr_name}',
                rf'emit_{instr_name}',
                rf'\b{instr_name}\b',
            ]
            
            for pattern in patterns:
                if re.search(pattern, backend_content, re.IGNORECASE):
                    self.instructions[instr_name].implemented = True
                    break
        
        return self._generate_report()
    
    def analyze_tests(self, test_dir: str) -> CoverageReport:
        """Analyze test files for SIMD usage"""
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
        """Analyze a single test file for SIMD-related code"""
        try:
            with open(filepath, 'r') as f:
                content = f.read()
        except IOError:
            return
        
        filename = os.path.basename(filepath)
        
        # Check for SIMD-related patterns
        simd_patterns = {
            'float': ['float', 'floats', 'floating'],
            'double': ['double', 'doubles'],
            'sse': ['sse', 'simd', 'vector'],
            'array': ['array', 'arrays', 'vectorize'],
        }
        
        for category, patterns in simd_patterns.items():
            for pattern in patterns:
                if pattern.lower() in content.lower():
                    # Mark relevant instructions as tested
                    for instr_name, instr in self.instructions.items():
                        if instr.category == category or category in instr.name:
                            instr.test_coverage.add(filename)
                    break
    
    def _generate_report(self) -> CoverageReport:
        """Generate coverage report"""
        report = CoverageReport()
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
        lines = []
        lines.append("=" * 70)
        lines.append("SIMD Instruction Coverage Report")
        lines.append("=" * 70)
        
        lines.append(f"\nTotal SIMD Instructions: {report.total_instructions}")
        lines.append(f"Implemented: {report.implemented_count} ({report.implemented_count/report.total_instructions*100:.1f}%)")
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
        description='Track SIMD instruction coverage in ZeroCC'
    )
    parser.add_argument('--check-backend', metavar='BACKEND_FILE',
                       help='Check backend implementation')
    parser.add_argument('--analyze-tests', metavar='TEST_DIR',
                       help='Analyze test directory')
    parser.add_argument('--report', action='store_true',
                       help='Generate report')
    parser.add_argument('--output', '-o', metavar='FILE',
                       help='Output file for report')
    
    args = parser.parse_args()
    
    tracker = SIMDCoverageTracker()
    report = CoverageReport()
    
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
