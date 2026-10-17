#!/usr/bin/env python3
"""
Backend Interface Analyzer

Analyzes the backend interface (isa_backend.h) and implementations
to verify ISA-agnostic design and identify missing implementations.

Usage:
    python3 backend_interface_analyzer.py
    python3 backend_interface_analyzer.py --report backend_report.txt
"""

import argparse
import os
import re
import sys
import json
from dataclasses import dataclass, field
from typing import List, Dict, Set, Optional
from pathlib import Path

@dataclass
class BackendFunction:
    """Represents a backend interface function"""
    name: str
    signature: str
    is_required: bool = True
    is_implemented: Dict[str, bool] = field(default_factory=dict)  # isa -> bool
    x86_64_impl: Optional[str] = None
    aarch64_impl: Optional[str] = None
    riscv_impl: Optional[str] = None

@dataclass
class BackendAnalysis:
    """Complete backend interface analysis"""
    functions: List[BackendFunction] = field(default_factory=list)
    missing_implementations: Dict[str, List[str]] = field(default_factory=dict)
    x86_specific_functions: List[str] = field(default_factory=list)
    aarch64_specific_functions: List[str] = field(default_factory=list)
    recommendations: List[str] = field(default_factory=list)

class BackendInterfaceAnalyzer:
    """Analyzes backend interface for ISA-agnostic design"""
    
    def __init__(self, source_dir: str = "src/zero_cc"):
        self.source_dir = Path(source_dir)
        self.functions: Dict[str, BackendFunction] = {}
        
    def analyze(self) -> BackendAnalysis:
        """Perform complete analysis"""
        print("Analyzing backend interface...")
        
        # Parse isa_backend.h for interface functions
        self._parse_interface()
        
        # Check implementations in each backend
        self._check_implementations("isa/x86_64.c", "x86_64")
        self._check_implementations("isa/aarch64.c", "aarch64")
        self._check_implementations("isa/riscv.c", "riscv")
        
        # Find missing implementations
        self._find_missing_implementations()
        
        # Identify ISA-specific functions
        self._identify_isa_specific_functions()
        
        # Generate recommendations
        self._generate_recommendations()
        
        return BackendAnalysis(
            functions=list(self.functions.values()),
            missing_implementations=self.missing_implementations,
            x86_specific_functions=self.x86_specific_functions,
            aarch64_specific_functions=self.aarch64_specific_functions,
            recommendations=self.recommendations
        )
    
    def _parse_interface(self):
        """Parse isa_backend.h for interface functions"""
        header_path = self.source_dir / "isa" / "isa_backend.h"
        
        if not header_path.exists():
            print(f"Warning: {header_path} not found")
            return
        
        with open(header_path, 'r') as f:
            content = f.read()
        
        # Find function pointer declarations in backend struct
        # Pattern: void (*function_name)(...)
        pattern = r'\(\s*\*\s*(\w+)\s*\)\s*\(([^)]*)\)'
        
        for match in re.finditer(pattern, content):
            func_name = match.group(1)
            params = match.group(2)
            
            # Skip non-function members
            if func_name.startswith('_') or func_name in ['int_regs', 'fp_regs', 'num_int_regs']:
                continue
            
            # Check if it's a function pointer (has params)
            if 'ZeroCCCompiler' in params or 'void' in params:
                self.functions[func_name] = BackendFunction(
                    name=func_name,
                    signature=f"{func_name}({params})"
                )
    
    def _check_implementations(self, impl_file: str, isa: str):
        """Check implementations for a specific ISA"""
        file_path = self.source_dir / impl_file
        
        if not file_path.exists():
            print(f"Warning: {file_path} not found")
            return
        
        with open(file_path, 'r') as f:
            content = f.read()
        
        for func_name in self.functions:
            # Check for static implementation
            impl_pattern = rf'static\s+void\s+{isa}_(\w+)\s*\('
            if re.search(impl_pattern, content):
                # Map implementation name to interface name
                impl_name = f"{isa}_{func_name}"
                if re.search(rf'static\s+void\s+{impl_name}\s*\(', content):
                    self.functions[func_name].is_implemented[isa] = True
                    
                    # Store implementation signature
                    impl_match = re.search(
                        rf'static\s+void\s+{impl_name}\s*\(([^)]*)\)',
                        content
                    )
                    if impl_match:
                        if isa == "x86_64":
                            self.functions[func_name].x86_64_impl = impl_match.group(1)
                        elif isa == "aarch64":
                            self.functions[func_name].aarch64_impl = impl_match.group(1)
                        elif isa == "riscv":
                            self.functions[func_name].riscv_impl = impl_match.group(1)
                else:
                    self.functions[func_name].is_implemented[isa] = False
            else:
                self.functions[func_name].is_implemented[isa] = False
    
    def _find_missing_implementations(self):
        """Find functions missing implementations"""
        self.missing_implementations = {}
        
        for func_name, func in self.functions.items():
            missing = []
            for isa in ['x86_64', 'aarch64', 'riscv']:
                if not func.is_implemented.get(isa, False):
                    missing.append(isa)
            
            if missing:
                self.missing_implementations[func_name] = missing
    
    def _identify_isa_specific_functions(self):
        """Identify functions that are ISA-specific"""
        self.x86_specific_functions = []
        self.aarch64_specific_functions = []
        
        for func_name, func in self.functions.items():
            implemented = [isa for isa, impl in func.is_implemented.items() if impl]
            
            if 'x86_64' in implemented and 'aarch64' not in implemented:
                self.x86_specific_functions.append(func_name)
            elif 'aarch64' in implemented and 'x86_64' not in implemented:
                self.aarch64_specific_functions.append(func_name)
    
    def _generate_recommendations(self):
        """Generate design recommendations"""
        self.recommendations = []
        
        # Check for missing implementations
        if self.missing_implementations:
            self.recommendations.append(
                f"WARNING: {len(self.missing_implementations)} backend functions missing implementations"
            )
            for func_name, missing in self.missing_implementations.items():
                self.recommendations.append(f"  - {func_name}: missing for {', '.join(missing)}")
        
        # Check for ISA-specific functions
        if self.x86_specific_functions:
            self.recommendations.append(
                f"CRITICAL: {len(self.x86_specific_functions)} x86-64 specific functions should be made ISA-agnostic"
            )
            for func_name in self.x86_specific_functions:
                self.recommendations.append(f"  - {func_name}")
        
        # Check callee-saved handling
        if 'emit_callee_save_loads' in self.functions:
            if self.functions['emit_callee_save_loads'].is_implemented.get('x86_64', False):
                self.recommendations.append(
                    "CRITICAL: emit_callee_save_loads is x86-64 specific. "
                    "This should be moved into emit_epilogue or made ISA-agnostic."
                )
        
        # Check for pattern mismatch
        prologue_funcs = ['emit_prologue', 'emit_epilogue', 'emit_leave']
        for func_name in prologue_funcs:
            if func_name in self.functions:
                func = self.functions[func_name]
                x86_impl = func.x86_64_impl or ""
                aarch64_impl = func.aarch64_impl or ""
                
                # Check for Pattern A/B mismatch
                if 'Pattern A' in x86_impl or 'Pattern A' in aarch64_impl:
                    pass  # Would need more sophisticated analysis


def generate_report(analysis: BackendAnalysis, output_file: Optional[str] = None) -> str:
    """Generate human-readable report"""
    lines = []
    lines.append("=" * 80)
    lines.append("BACKEND INTERFACE ANALYSIS REPORT")
    lines.append("=" * 80)
    lines.append("")
    
    # Summary
    lines.append("SUMMARY")
    lines.append("-" * 80)
    lines.append(f"Total interface functions: {len(analysis.functions)}")
    lines.append(f"Missing implementations: {len(analysis.missing_implementations)}")
    lines.append(f"x86-64 specific: {len(analysis.x86_specific_functions)}")
    lines.append(f"AArch64 specific: {len(analysis.aarch64_specific_functions)}")
    lines.append("")
    
    # Implementation matrix
    lines.append("IMPLEMENTATION MATRIX")
    lines.append("-" * 80)
    lines.append(f"{'Function':<30} {'x86_64':<10} {'aarch64':<10} {'riscv':<10}")
    lines.append("-" * 60)
    
    for func in analysis.functions:
        x86 = "✓" if func.is_implemented.get('x86_64', False) else "✗"
        aarch64 = "✓" if func.is_implemented.get('aarch64', False) else "✗"
        riscv = "✓" if func.is_implemented.get('riscv', False) else "✗"
        lines.append(f"{func.name:<30} {x86:<10} {aarch64:<10} {riscv:<10}")
    
    lines.append("")
    
    # Missing implementations
    if analysis.missing_implementations:
        lines.append("MISSING IMPLEMENTATIONS")
        lines.append("-" * 80)
        for func_name, missing in analysis.missing_implementations.items():
            lines.append(f"  {func_name}: missing for {', '.join(missing)}")
        lines.append("")
    
    # ISA-specific functions
    if analysis.x86_specific_functions:
        lines.append("X86-64 SPECIFIC FUNCTIONS (CRITICAL)")
        lines.append("-" * 80)
        for func_name in analysis.x86_specific_functions:
            lines.append(f"  • {func_name}")
        lines.append("")
    
    if analysis.aarch64_specific_functions:
        lines.append("AArch64 SPECIFIC FUNCTIONS")
        lines.append("-" * 80)
        for func_name in analysis.aarch64_specific_functions:
            lines.append(f"  • {func_name}")
        lines.append("")
    
    # Recommendations
    if analysis.recommendations:
        lines.append("RECOMMENDATIONS")
        lines.append("-" * 80)
        for rec in analysis.recommendations:
            lines.append(f"  {rec}")
        lines.append("")
    
    report = "\n".join(lines)
    
    if output_file:
        with open(output_file, 'w') as f:
            f.write(report)
        print(f"Report saved to: {output_file}")
    
    return report


def main():
    parser = argparse.ArgumentParser(
        description='Analyze backend interface for ISA-agnostic design'
    )
    parser.add_argument('--source-dir', default='src/zero_cc',
                       help='Source directory')
    parser.add_argument('--report', help='Output report file')
    parser.add_argument('--json', help='Output JSON analysis')
    
    args = parser.parse_args()
    
    if not os.path.exists(args.source_dir):
        print(f"Error: Source directory not found: {args.source_dir}")
        sys.exit(1)
    
    analyzer = BackendInterfaceAnalyzer(args.source_dir)
    analysis = analyzer.analyze()
    
    # Generate report
    report = generate_report(analysis, args.report)
    print(report)
    
    # Export JSON if requested
    if args.json:
        json_data = {
            'functions': [
                {
                    'name': func.name,
                    'signature': func.signature,
                    'implemented': func.is_implemented,
                    'x86_64_impl': func.x86_64_impl,
                    'aarch64_impl': func.aarch64_impl,
                    'riscv_impl': func.riscv_impl
                }
                for func in analysis.functions
            ],
            'missing_implementations': analysis.missing_implementations,
            'x86_specific': analysis.x86_specific_functions,
            'aarch64_specific': analysis.aarch64_specific_functions,
            'recommendations': analysis.recommendations
        }
        with open(args.json, 'w') as f:
            json.dump(json_data, f, indent=2)
        print(f"\nJSON saved to: {args.json}")


if __name__ == '__main__':
    main()
