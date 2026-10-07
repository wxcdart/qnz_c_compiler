#!/usr/bin/env python3
"""
AST Lowering Design Analyzer

Analyzes the AST lowering layer to identify ISA-specific assumptions
vs ISA-agnostic code. Helps verify our ISA-agnostic IR design goals.

Usage:
    python3 ast_lowering_analyzer.py --analyze-dir src/zero_cc/parser/
    python3 ast_lowering_analyzer.py --compare-isas x86_64 aarch64
    python3 ast_lowering_analyzer.py --report design_report.txt
"""

import argparse
import os
import re
import json
from dataclasses import dataclass, field
from typing import List, Dict, Set, Tuple, Optional
from pathlib import Path

@dataclass
class ISAAssumption:
    """Represents an ISA-specific assumption in the code"""
    file: str
    line: int
    code: str
    assumption_type: str  # "register", "stack_layout", "calling_convention", "instruction", "other"
    isa: str  # "x86_64", "aarch64", "riscv", "all"
    severity: str  # "critical", "warning", "info"
    description: str

@dataclass
class LoweringPattern:
    """Represents a code pattern in AST lowering"""
    name: str
    files: List[str] = field(default_factory=list)
    lines: List[int] = field(default_factory=list)
    is_isa_agnostic: bool = True
    description: str = ""

@dataclass
class DesignAnalysis:
    """Complete design analysis result"""
    isa_assumptions: List[ISAAssumption] = field(default_factory=list)
    patterns: List[LoweringPattern] = field(default_factory=list)
    files_analyzed: List[str] = field(default_factory=list)
    summary: Dict = field(default_factory=dict)

class ASTLoweringAnalyzer:
    """Analyzes AST lowering layer for ISA-specific assumptions"""
    
    # x86-64 specific patterns
    X86_64_PATTERNS = [
        (r'rbx|r12|r13|r14|r15', 'register', 'x86-64 callee-saved registers'),
        (r'rax|rdi|rsi|rdx|rcx|r8|r9|r10|r11', 'register', 'x86-64 register names'),
        (r'rex\s', 'instruction', 'x86-64 REX prefix'),
        (r'lea\s', 'instruction', 'x86-64 LEA instruction'),
        (r'push\s|pop\s', 'instruction', 'x86-64 push/pop'),
        (r'sub.*rsp', 'stack_layout', 'x86-64 stack pointer manipulation'),
        (r'rbp', 'register', 'x86-64 base pointer'),
        (r'CALLEE_SAVE', 'stack_layout', 'x86-64 callee-saved frame layout'),
        (r'emit_callee_save', 'calling_convention', 'x86-64 callee-saved preservation'),
    ]
    
    # AArch64 specific patterns
    AARCH64_PATTERNS = [
        (r'\bx[0-9]+\b', 'register', 'AArch64 general-purpose registers'),
        (r'\bv[0-9]+\b', 'register', 'AArch64 vector registers'),
        (r'\bx29\b', 'register', 'AArch64 frame pointer'),
        (r'\bx30\b', 'register', 'AArch64 link register'),
        (r'stp\s|ldp\s', 'instruction', 'AArch64 pair load/store'),
        (r'sub\s+sp', 'stack_layout', 'AArch64 stack manipulation'),
    ]
    
    # ISA-agnostic patterns (good)
    ISA_AGNOSTIC_PATTERNS = [
        (r'BE\(c\)->emit_', 'instruction', 'Backend virtual function call'),
        (r'ir_emit_', 'instruction', 'ISA-neutral IR emission'),
        (r'VREG_', 'register', 'Virtual register'),
        (r'sym->stack_offset', 'stack_layout', 'Symbol stack offset (ISA-neutral)'),
    ]
    
    def __init__(self, source_dir: str):
        self.source_dir = Path(source_dir)
        self.assumptions: List[ISAAssumption] = []
        self.patterns: Dict[str, LoweringPattern] = {}
        self.files_analyzed: List[str] = []
        
    def analyze(self) -> DesignAnalysis:
        """Perform complete analysis"""
        print(f"Analyzing AST lowering layer in {self.source_dir}...")
        
        # Find all C source files
        c_files = list(self.source_dir.glob('*.c')) + list(self.source_dir.glob('*.h'))
        
        for c_file in c_files:
            self._analyze_file(c_file)
        
        # Generate summary
        summary = self._generate_summary()
        
        return DesignAnalysis(
            isa_assumptions=self.assumptions,
            patterns=list(self.patterns.values()),
            files_analyzed=self.files_analyzed,
            summary=summary
        )
    
    def _analyze_file(self, file_path: Path):
        """Analyze a single file for ISA-specific patterns"""
        try:
            with open(file_path, 'r') as f:
                lines = f.readlines()
        except Exception as e:
            print(f"  Warning: Could not read {file_path}: {e}")
            return
        
        self.files_analyzed.append(str(file_path))
        rel_path = str(file_path.relative_to(self.source_dir))
        
        # Check for ISA-specific patterns
        for pattern, pattern_type, description in self.X86_64_PATTERNS:
            for line_num, line in enumerate(lines, 1):
                if re.search(pattern, line, re.IGNORECASE):
                    self.assumptions.append(ISAAssumption(
                        file=rel_path,
                        line=line_num,
                        code=line.strip()[:100],
                        assumption_type=pattern_type,
                        isa='x86_64',
                        severity='critical' if pattern_type in ['register', 'stack_layout'] else 'warning',
                        description=description
                    ))
        
        for pattern, pattern_type, description in self.AARCH64_PATTERNS:
            for line_num, line in enumerate(lines, 1):
                if re.search(pattern, line, re.IGNORECASE):
                    self.assumptions.append(ISAAssumption(
                        file=rel_path,
                        line=line_num,
                        code=line.strip()[:100],
                        assumption_type=pattern_type,
                        isa='aarch64',
                        severity='warning',
                        description=description
                    ))
        
        # Check for ISA-agnostic patterns (good)
        for pattern, pattern_type, description in self.ISA_AGNOSTIC_PATTERNS:
            for line_num, line in enumerate(lines, 1):
                if re.search(pattern, line):
                    pattern_name = f"ISA-agnostic: {description}"
                    if pattern_name not in self.patterns:
                        self.patterns[pattern_name] = LoweringPattern(
                            name=pattern_name,
                            description=description,
                            is_isa_agnostic=True
                        )
                    self.patterns[pattern_name].files.append(rel_path)
                    self.patterns[pattern_name].lines.append(line_num)
    
    def _generate_summary(self) -> Dict:
        """Generate analysis summary"""
        x86_assumptions = [a for a in self.assumptions if a.isa == 'x86_64']
        aarch64_assumptions = [a for a in self.assumptions if a.isa == 'aarch64']
        critical = [a for a in self.assumptions if a.severity == 'critical']
        
        return {
            'total_files': len(self.files_analyzed),
            'total_assumptions': len(self.assumptions),
            'x86_64_assumptions': len(x86_assumptions),
            'aarch64_assumptions': len(aarch64_assumptions),
            'critical_issues': len(critical),
            'isa_agnostic_patterns': len([p for p in self.patterns.values() if p.is_isa_agnostic]),
            'assumptions_by_type': self._count_by_type(),
            'recommendations': self._generate_recommendations()
        }
    
    def _count_by_type(self) -> Dict[str, int]:
        """Count assumptions by type"""
        counts = {}
        for a in self.assumptions:
            counts[a.assumption_type] = counts.get(a.assumption_type, 0) + 1
        return counts
    
    def _generate_recommendations(self) -> List[str]:
        """Generate design recommendations"""
        recommendations = []
        
        # Check for callee-saved issues
        callee_save_issues = [a for a in self.assumptions if 'callee_save' in a.description.lower()]
        if callee_save_issues:
            recommendations.append(
                "CRITICAL: Callee-saved register handling is x86-64 specific. "
                "Move to backend layer or make ISA-agnostic."
            )
        
        # Check for register name issues
        register_issues = [a for a in self.assumptions if a.assumption_type == 'register']
        if register_issues:
            recommendations.append(
                "WARNING: Found ISA-specific register names. "
                "Use virtual registers (VREG_) and backend interface instead."
            )
        
        # Check for stack layout issues
        stack_issues = [a for a in self.assumptions if a.assumption_type == 'stack_layout']
        if stack_issues:
            recommendations.append(
                "WARNING: Found ISA-specific stack layout assumptions. "
                "Ensure stack_offset is used consistently across ISAs."
            )
        
        return recommendations


def generate_report(analysis: DesignAnalysis, output_file: Optional[str] = None) -> str:
    """Generate human-readable report"""
    lines = []
    lines.append("=" * 80)
    lines.append("AST LOWERING DESIGN ANALYSIS REPORT")
    lines.append("=" * 80)
    lines.append("")
    
    # Summary
    lines.append("SUMMARY")
    lines.append("-" * 80)
    lines.append(f"Files analyzed: {analysis.summary['total_files']}")
    lines.append(f"Total ISA assumptions found: {analysis.summary['total_assumptions']}")
    lines.append(f"  - x86-64 specific: {analysis.summary['x86_64_assumptions']}")
    lines.append(f"  - AArch64 specific: {analysis.summary['aarch64_assumptions']}")
    lines.append(f"Critical issues: {analysis.summary['critical_issues']}")
    lines.append(f"ISA-agnostic patterns: {analysis.summary['isa_agnostic_patterns']}")
    lines.append("")
    
    # Assumptions by type
    lines.append("ASSUMPTIONS BY TYPE")
    lines.append("-" * 80)
    for atype, count in analysis.summary['assumptions_by_type'].items():
        lines.append(f"  {atype}: {count}")
    lines.append("")
    
    # Critical issues
    critical = [a for a in analysis.isa_assumptions if a.severity == 'critical']
    if critical:
        lines.append("CRITICAL ISSUES")
        lines.append("-" * 80)
        for issue in critical:
            lines.append(f"[{issue.isa}] {issue.file}:{issue.line}")
            lines.append(f"  {issue.description}")
            lines.append(f"  Code: {issue.code}")
            lines.append("")
    
    # All assumptions
    lines.append("ALL ISA-SPECIFIC ASSUMPTIONS")
    lines.append("-" * 80)
    for issue in analysis.isa_assumptions:
        severity_marker = "[CRITICAL]" if issue.severity == 'critical' else "[WARN]"
        lines.append(f"{severity_marker} [{issue.isa}] {issue.file}:{issue.line}")
        lines.append(f"  Type: {issue.assumption_type}")
        lines.append(f"  {issue.description}")
        lines.append(f"  Code: {issue.code}")
        lines.append("")
    
    # Recommendations
    lines.append("RECOMMENDATIONS")
    lines.append("-" * 80)
    for rec in analysis.summary['recommendations']:
        lines.append(f"• {rec}")
        lines.append("")
    
    # ISA-agnostic patterns
    lines.append("ISA-AGNOSTIC PATTERNS (GOOD)")
    lines.append("-" * 80)
    for pattern in analysis.patterns:
        if pattern.is_isa_agnostic:
            lines.append(f"  {pattern.name}")
            lines.append(f"    Found in: {', '.join(pattern.files[:5])}")
            if len(pattern.files) > 5:
                lines.append(f"    ... and {len(pattern.files) - 5} more files")
    lines.append("")
    
    report = "\n".join(lines)
    
    if output_file:
        with open(output_file, 'w') as f:
            f.write(report)
        print(f"Report saved to: {output_file}")
    
    return report


def main():
    parser = argparse.ArgumentParser(
        description='Analyze AST lowering layer for ISA-specific assumptions'
    )
    parser.add_argument('--analyze-dir', default='src/zero_cc/parser/',
                       help='Directory to analyze')
    parser.add_argument('--report', help='Output report file')
    parser.add_argument('--json', help='Output JSON analysis')
    parser.add_argument('--verbose', '-v', action='store_true',
                       help='Verbose output')
    
    args = parser.parse_args()
    
    if not os.path.exists(args.analyze_dir):
        print(f"Error: Directory not found: {args.analyze_dir}")
        sys.exit(1)
    
    analyzer = ASTLoweringAnalyzer(args.analyze_dir)
    analysis = analyzer.analyze()
    
    # Generate report
    report = generate_report(analysis, args.report)
    print(report)
    
    # Export JSON if requested
    if args.json:
        json_data = {
            'summary': analysis.summary,
            'assumptions': [
                {
                    'file': a.file,
                    'line': a.line,
                    'code': a.code,
                    'type': a.assumption_type,
                    'isa': a.isa,
                    'severity': a.severity,
                    'description': a.description
                }
                for a in analysis.isa_assumptions
            ],
            'patterns': [
                {
                    'name': p.name,
                    'files': p.files,
                    'is_isa_agnostic': p.is_isa_agnostic,
                    'description': p.description
                }
                for p in analysis.patterns
            ]
        }
        with open(args.json, 'w') as f:
            json.dump(json_data, f, indent=2)
        print(f"\nJSON data saved to: {args.json}")
    
    # Exit with error if critical issues found
    if analysis.summary['critical_issues'] > 0:
        sys.exit(1)


if __name__ == '__main__':
    import sys
    main()
