#!/usr/bin/env python3
"""
Backend Coverage Analyzer for ZeroCC - Task 67

Tracks which ISA backend functions are implemented vs missing.
"""

import argparse
import re
from pathlib import Path
from dataclasses import dataclass
from typing import Dict, List, Set, Tuple

@dataclass
class BackendFunction:
    name: str
    category: str
    description: str
    required_by: List[str]
    implemented: bool = False

BACKEND_CATEGORIES = {
    "core_emission": [
        ("emit_mov_reg", "Register move", ["all"]),
        ("emit_add", "Addition", ["arithmetic"]),
        ("emit_sub", "Subtraction", ["arithmetic"]),
        ("emit_mul", "Multiplication", ["arithmetic"]),
        ("emit_div", "Division", ["arithmetic"]),
        ("emit_mod", "Modulo", ["arithmetic"]),
        ("emit_neg", "Negation", ["unary"]),
        ("emit_and", "Bitwise AND", ["bitwise"]),
        ("emit_or", "Bitwise OR", ["bitwise"]),
        ("emit_xor", "Bitwise XOR", ["bitwise"]),
        ("emit_not", "Bitwise NOT", ["bitwise"]),
        ("emit_shl", "Shift left", ["shift"]),
        ("emit_shr", "Shift right", ["shift"]),
    ],
    "memory_ops": [
        ("emit_deref", "Dereference", ["all deref"]),
        ("emit_load", "64-bit load", ["memory"]),
        ("emit_load_32", "32-bit load", ["memory"]),
        ("emit_load_16", "16-bit load", ["memory"]),
        ("emit_load_byte", "8-bit load", ["memory"]),
        ("emit_store", "64-bit store", ["memory"]),
        ("emit_store_32", "32-bit store", ["memory"]),
        ("emit_store_16", "16-bit store", ["memory"]),
        ("emit_store_byte", "8-bit store", ["memory"]),
        ("emit_lea", "LEA", ["address-of"]),
        ("emit_lea_rip", "RIP-relative LEA", ["globals"]),
    ],
    "comparisons": [
        ("emit_cmp", "Compare", ["comparisons"]),
        ("emit_setl", "Set if less", ["comparisons"]),
        ("emit_setle", "Set if <= ", ["comparisons"]),
        ("emit_setg", "Set if greater", ["comparisons"]),
        ("emit_setge", "Set if >= ", ["comparisons"]),
        ("emit_sete", "Set if equal", ["comparisons"]),
        ("emit_setne", "Set if != ", ["comparisons"]),
        ("emit_cmp_zero", "Compare to zero", ["comparisons"]),
        ("emit_lnot", "Logical NOT", ["boolean"]),
    ],
    "control_flow": [
        ("emit_label", "Label", ["control"]),
        ("emit_jmp", "Jump", ["control"]),
        ("emit_jeq", "Jump if equal", ["control"]),
        ("emit_jne", "Jump if not equal", ["control"]),
        ("emit_jlt", "Jump if less", ["control"]),
        ("emit_jle", "Jump if less or equal", ["control"]),
        ("emit_jgt", "Jump if greater", ["control"]),
        ("emit_jge", "Jump if greater or equal", ["control"]),
        ("emit_call", "Call", ["functions"]),
        ("emit_ret", "Return", ["functions"]),
    ],
    "function_abi": [
        ("emit_prologue", "Prologue", ["functions"]),
        ("emit_epilogue", "Epilogue", ["functions"]),
        ("emit_param_save_int", "Save int param", ["variadic"]),
        ("emit_param_save_fp", "Save FP param", ["variadic"]),
        ("emit_variadic_spill", "Variadic spill", ["variadic"]),
        ("emit_variadic_al_setup", "Variadic setup", ["variadic"]),
    ],
    "special_ops": [
        ("emit_vla_alloc", "VLA alloc", ["VLAs"]),
        ("emit_atomic_cmpxchg", "Atomic cmpxchg", ["atomics"]),
        ("emit_atomic_xadd", "Atomic xadd", ["atomics"]),
        ("emit_atomic_xchg", "Atomic xchg", ["atomics"]),
        ("emit_mfence", "MFence", ["atomics"]),
        ("emit_fload", "FP load", ["fp"]),
        ("emit_fstore", "FP store", ["fp"]),
        ("emit_fadd", "FP add", ["fp"]),
        ("emit_fsub", "FP sub", ["fp"]),
        ("emit_fmul", "FP mul", ["fp"]),
        ("emit_fdiv", "FP div", ["fp"]),
        ("emit_fmov_imm", "FP move imm", ["fp"]),
        ("emit_fcmp", "FP cmp", ["fp"]),
    ],
}


class BackendCoverageAnalyzer:
    def __init__(self, base_dir: str):
        self.base_dir = Path(base_dir)
        self.isa_dir = self.base_dir / "src" / "zero_cc" / "isa"
    
    def scan_backend_file(self, file_path: Path) -> Set[str]:
        implemented = set()
        if not file_path.exists():
            return implemented
        
        with open(file_path, 'r') as f:
            content = f.read()
        
        # Map different naming conventions to standard interface names
        name_mappings = {
            # Core emission
            'mov_reg': 'mov_reg',
            'add': 'add', 'sub': 'sub', 'mul': 'mul', 'div': 'div', 'mod': 'mod',
            'neg': 'neg', 'and': 'and', 'or': 'or', 'xor': 'xor', 'not': 'not',
            'shl': 'shl', 'shr': 'shr',
            # Memory ops
            'load': 'load', 'load_32': 'load_32', 'load_16': 'load_16', 'load_byte': 'load_byte',
            'store': 'store', 'store_32': 'store_32', 'store_16': 'store_16', 'store_byte': 'store_byte',
            'lea': 'lea', 'lea_rip': 'lea_rip', 'deref': 'deref',
            # FP operations (x86 uses load_fp/store_fp, fp_add, etc.)
            'load_fp': 'fload', 'store_fp': 'fstore',
            'fp_add': 'fadd', 'fp_sub': 'fsub', 'fp_mul': 'fmul', 'fp_div': 'fdiv',
            'fp_mov': 'fmov_imm', 'fp_cmp': 'fcmp',
            # Comparisons (x86 uses setl, setle, etc. directly)
            'cmp': 'cmp', 'setl': 'setl', 'setle': 'setle', 'setg': 'setg',
            'setge': 'setge', 'sete': 'sete', 'setne': 'setne',
            'cmp_zero': 'cmp_zero', 'lnot': 'lnot',
            # Control flow (x86 uses jmp, label, call, ret - no jmp_cond)
            'label': 'label', 'jmp': 'jmp',
            'jeq': 'jeq', 'jne': 'jne', 'jlt': 'jlt', 'jle': 'jle',
            'jgt': 'jgt', 'jge': 'jge',
            'call': 'call', 'call_reg': 'call', 'ret': 'ret', 'nop': 'nop',
            # Function ABI
            'prologue': 'prologue', 'epilogue': 'epilogue',
            'param_save_int': 'param_save_int', 'param_save_fp': 'param_save_fp',
            'variadic_spill': 'variadic_spill', 'variadic_al_setup': 'variadic_al_setup',
            # Special ops
            'vla_alloc': 'vla_alloc', 'mfence': 'mfence',
            'atomic_cmpxchg': 'atomic_cmpxchg', 'atomic_xadd': 'atomic_xadd',
            'atomic_xchg': 'atomic_xchg',
            # x86-specific: mov_rr is register move
            'mov_rr': 'mov_reg',
        }
        
        # Match patterns: x86_emit_*, aarch64_emit_*, riscv_emit_*, me_emit_*
        patterns = [
            r'static\s+\w+\s+\w+_emit_(\w+)\s*\(',
            r'static\s+\w+\s+(\w+)_emit_(\w+)\s*\(',  # x86_emit_foo -> foo
            r'static\s+\w+\s+x86_(\w+)\s*\(',  # x86_mov_rr -> mov_rr
            r'static\s+\w+\s+aarch64_(\w+)\s*\(',  # aarch64_foo -> foo
            r'static\s+\w+\s+riscv_(\w+)\s*\(',  # riscv_foo -> foo
            r'me_emit_(\w+)\s*\(',
        ]
        
        for pattern in patterns:
            matches = re.finditer(pattern, content)
            for match in matches:
                # Handle patterns with different group counts
                if match.lastindex == 2:  # Two groups: isa and func
                    raw_name = match.group(2)
                elif match.lastindex == 1:  # One group: func
                    raw_name = match.group(1)
                else:
                    continue
                # Map to standard interface name
                if raw_name in name_mappings:
                    implemented.add(f"emit_{name_mappings[raw_name]}")
        return implemented
    
    def analyze_isa(self, isa_name: str) -> Dict[str, BackendFunction]:
        backend_file = self.isa_dir / f"{isa_name}.c"
        implemented = self.scan_backend_file(backend_file)
        coverage = {}
        for category, funcs in BACKEND_CATEGORIES.items():
            for func_name, description, required_by in funcs:
                is_impl = func_name in implemented
                coverage[func_name] = BackendFunction(
                    name=func_name,
                    category=category,
                    description=description,
                    required_by=required_by,
                    implemented=is_impl,
                )
        return coverage
    
    def generate_report(self, isa_name: str = "all") -> str:
        if isa_name == "all":
            isas = ["x86_64", "aarch64", "riscv"]
        else:
            isas = [isa_name]
        
        report_lines = ["=" * 70, "ZeroCC Backend Coverage Report", "=" * 70]
        
        for isa in isas:
            coverage = self.analyze_isa(isa)
            total = len(coverage)
            implemented = sum(1 for f in coverage.values() if f.implemented)
            missing = total - implemented
            pct = (implemented / total) * 100 if total > 0 else 0
            
            report_lines.append(f"\n{isa.upper()} Backend:")
            report_lines.append(f"  Implemented: {implemented}/{total} ({pct:.1f}%)")
            report_lines.append(f"  Missing: {missing}")
            
            categories = {}
            for func in coverage.values():
                if func.category not in categories:
                    categories[func.category] = {"impl": [], "missing": []}
                if func.implemented:
                    categories[func.category]["impl"].append(func.name)
                else:
                    categories[func.category]["missing"].append(func.name)
            
            for cat, funcs in categories.items():
                if funcs["missing"]:
                    report_lines.append(f"\n  [{cat}] Missing ({len(funcs['missing'])}):")
                    for func_name in funcs["missing"]:
                        report_lines.append(f"    - {func_name}")
        
        report_lines.append("\n" + "=" * 70)
        return "\n".join(report_lines)


def main():
    parser = argparse.ArgumentParser(description="Backend Coverage Analyzer")
    parser.add_argument("--isa", choices=["x86_64", "aarch64", "riscv", "all"], default="all")
    parser.add_argument("--generate-report", action="store_true")
    args = parser.parse_args()
    
    base_dir = Path(__file__).parent.parent
    analyzer = BackendCoverageAnalyzer(str(base_dir))
    
    if args.generate_report:
        print(analyzer.generate_report(args.isa))
    else:
        print(analyzer.generate_report(args.isa))


if __name__ == "__main__":
    main()
