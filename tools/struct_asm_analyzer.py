#!/usr/bin/env python3
"""
Struct Assembly Analyzer for ZeroCC - Phase 2

Analyzes generated assembly to verify correct struct handling:
- Argument passing (register vs stack vs hidden pointer)
- Return value handling (registers vs hidden pointer)
- Stack frame layout for structs
- Struct member access patterns

Usage:
    python3 tools/struct_asm_analyzer.py --asm file.s --analyze-structs
    python3 tools/struct_asm_analyzer.py --source test.c --analyze-calls
    python3 tools/struct_asm_analyzer.py --verbose --full-analysis
"""

import argparse
import subprocess
import sys
import re
import tempfile
import os
from pathlib import Path
from dataclasses import dataclass, field
from typing import List, Dict, Optional, Tuple

@dataclass
class StructAccess:
    """Struct member access pattern."""
    instruction: str
    offset: int
    size: int
    base_reg: str
    direction: str  # 'load' or 'store'

@dataclass
class FunctionCall:
    """Function call analysis."""
    func_name: str
    arg_registers: List[str] = field(default_factory=list)
    arg_stack_offsets: List[int] = field(default_factory=list)
    uses_hidden_ptr: bool = False
    return_reg: str = ""

@dataclass
class StructInfo:
    """Struct information extracted from assembly."""
    name: str
    size: int
    alignment: int
    member_offsets: Dict[str, int] = field(default_factory=dict)
    chunks: int = 1

@dataclass
class AnalysisResult:
    """Result of assembly analysis."""
    source_file: str
    functions: List[FunctionCall] = field(default_factory=list)
    structs: List[StructInfo] = field(default_factory=list)
    struct_accesses: List[StructAccess] = field(default_factory=list)
    issues: List[str] = field(default_factory=list)
    warnings: List[str] = field(default_factory=list)


def compile_to_asm(source_code: str) -> Tuple[Optional[str], Optional[str]]:
    """Compile source code to assembly."""
    with tempfile.NamedTemporaryFile(mode='w', suffix='.c', delete=False) as f:
        f.write(source_code)
        src_file = f.name
    
    try:
        result = subprocess.run(
            ['./build/bin/zerocc', '-S', src_file, '-o', '-'],
            capture_output=True, text=True, cwd='/home/ubuntu/dev/qnz_c_compiler'
        )
        
        if result.returncode != 0:
            return None, result.stderr
        
        return result.stdout, None
    finally:
        os.unlink(src_file)


def parse_struct_comments(asm: str) -> List[StructInfo]:
    """Parse struct information from assembly comments."""
    structs = []
    
    # Look for struct size comments (ZeroCC emits these)
    struct_pattern = re.compile(
        r'#.*struct\s+(\w+).*size:\s*(\d+).*align:\s*(\d+)',
        re.IGNORECASE
    )
    
    for match in struct_pattern.finditer(asm):
        name = match.group(1)
        size = int(match.group(2))
        align = int(match.group(3))
        chunks = (size + 7) // 8
        
        structs.append(StructInfo(
            name=name,
            size=size,
            alignment=align,
            chunks=chunks
        ))
    
    return structs


def extract_function_body(asm: str, func_name: str) -> Optional[str]:
    """Extract assembly for a specific function."""
    # Find function start
    patterns = [
        rf'^{re.escape(func_name)}:\s*$',
        rf'^\t.globl\s+{re.escape(func_name)}\s*$',
    ]
    
    start_idx = -1
    for pattern in patterns:
        match = re.search(pattern, asm, re.MULTILINE)
        if match:
            start_idx = match.end()
            break
    
    if start_idx == -1:
        return None
    
    # Find next function or label
    remaining = asm[start_idx:]
    end_match = re.search(r'^\s*(?:\.globl|\w+:)', remaining, re.MULTILINE)
    
    if end_match:
        return remaining[:end_match.start()]
    else:
        return remaining


def analyze_argument_registers(func_asm: str, func_name: str) -> FunctionCall:
    """Analyze which registers are used for function arguments."""
    call = FunctionCall(func_name=func_name)
    
    # Standard argument registers (System V AMD64 ABI)
    arg_regs = ['rdi', 'rsi', 'rdx', 'rcx', 'r8', 'r9']
    fp_arg_regs = ['xmm0', 'xmm1', 'xmm2', 'xmm3', 'xmm4', 'xmm5', 'xmm6', 'xmm7']
    
    # Look at first N instructions for argument usage
    lines = func_asm.split('\n')[:30]
    prologue_end = 0
    
    for i, line in enumerate(lines):
        # Skip prologue
        if 'pushq %rbp' in line or 'movq %rsp,%rbp' in line:
            prologue_end = i + 1
            continue
        
        if i < prologue_end + 5:  # Check first few instructions after prologue
            line_lower = line.lower()
            
            # Check for argument register usage
            for reg in arg_regs:
                if f'%{reg}' in line_lower and 'mov' in line_lower:
                    if reg not in call.arg_registers:
                        call.arg_registers.append(reg)
            
            for reg in fp_arg_regs:
                if f'%{reg}' in line_lower and 'mov' in line_lower:
                    if reg not in call.arg_registers:
                        call.arg_registers.append(reg)
    
    return call


def analyze_return_value(func_asm: str) -> str:
    """Analyze which register is used for return value."""
    lines = func_asm.split('\n')
    
    # Look for return value setup before ret
    for i in range(len(lines) - 1, -1, -1):
        line = lines[i].strip()
        
        # Skip epilogue
        if 'leave' in line or 'ret' in line:
            continue
        
        # Check for return register usage
        if re.search(r'movq\s+.*,%rax', line):
            return 'rax'
        if re.search(r'movq\s+.*,%rdx', line):
            return 'rdx'
        if re.search(r'movsd\s+.*,%xmm0', line):
            return 'xmm0'
    
    return ''


def analyze_struct_accesses(func_asm: str) -> List[StructAccess]:
    """Analyze struct member access patterns."""
    accesses = []
    
    # Pattern: mov offset(base_reg), dest (load)
    load_pattern = re.compile(r'movq\s+(-?\d*)\(%(\w+)\),\s*%(\w+)')
    
    # Pattern: mov src, offset(base_reg) (store)
    store_pattern = re.compile(r'movq\s+%(\w+),\s+(-?\d*)\(%(\w+)\)')
    
    for line in func_asm.split('\n'):
        # Check for loads
        match = load_pattern.search(line)
        if match:
            offset_str = match.group(1)
            offset = int(offset_str) if offset_str and offset_str != '' else 0
            base_reg = match.group(2)
            
            # Only consider frame pointer or argument pointer accesses
            if base_reg in ['rbp', 'rsp', 'rdi']:
                accesses.append(StructAccess(
                    instruction=line.strip(),
                    offset=offset,
                    size=8,
                    base_reg=base_reg,
                    direction='load'
                ))
        
        # Check for stores
        match = store_pattern.search(line)
        if match:
            offset_str = match.group(2)
            offset = int(offset_str) if offset_str and offset_str != '' else 0
            base_reg = match.group(3)
            
            if base_reg in ['rbp', 'rsp', 'rdi']:
                accesses.append(StructAccess(
                    instruction=line.strip(),
                    offset=offset,
                    size=8,
                    base_reg=base_reg,
                    direction='store'
                ))
    
    return accesses


def analyze_hidden_pointer_usage(func_asm: str, func_name: str) -> bool:
    """Check if function uses hidden pointer for struct."""
    # Hidden pointer is typically in rdi (first argument position)
    # Look for stores to [rdi+offset]
    pattern = re.compile(r'movq\s+%\w+,\s*-?\d*\(%%?rdi\)')
    
    return bool(pattern.search(func_asm))


def analyze_all_functions(asm: str) -> AnalysisResult:
    """Analyze all functions in assembly."""
    result = AnalysisResult(source_file='unknown')
    
    # Parse struct definitions
    result.structs = parse_struct_comments(asm)
    
    # Find all functions
    func_pattern = re.compile(r'^\.globl\s+(\w+)$', re.MULTILINE)
    
    for match in func_pattern.finditer(asm):
        func_name = match.group(1)
        
        # Skip internal functions
        if func_name in ['main', '__libc_start_main', '__main']:
            continue
        
        # Extract function body
        func_asm = extract_function_body(asm, func_name)
        if not func_asm:
            continue
        
        # Analyze arguments
        call = analyze_argument_registers(func_asm, func_name)
        
        # Analyze return value
        call.return_reg = analyze_return_value(func_asm)
        
        # Check for hidden pointer
        call.uses_hidden_ptr = analyze_hidden_pointer_usage(func_asm, func_name)
        
        result.functions.append(call)
        
        # Analyze struct accesses
        accesses = analyze_struct_accesses(func_asm)
        result.struct_accesses.extend(accesses)
    
    return result


def print_analysis(result: AnalysisResult, verbose: bool = False):
    """Print analysis results."""
    print("\n" + "=" * 70)
    print("STRUCT ASSEMBLY ANALYSIS")
    print("=" * 70)
    
    # Struct definitions
    if result.structs:
        print("\n[Struct Definitions]")
        for struct in result.structs:
            chunks = struct.chunks
            passing = "registers" if chunks <= 2 else "hidden pointer"
            print(f"  {struct.name}: {struct.size} bytes ({chunks} chunk(s)) -> {passing}")
    
    # Function calls
    if result.functions:
        print("\n[Function Calls]")
        for func in result.functions:
            print(f"\n  {func.name}:")
            
            if func.arg_registers:
                print(f"    Arg registers: {', '.join(func.arg_registers)}")
            
            if func.uses_hidden_ptr:
                print(f"    Uses hidden pointer: YES")
            
            if func.return_reg:
                print(f"    Return register: {func.return_reg}")
    
    # Struct accesses
    if result.struct_accesses and verbose:
        print("\n[Struct Member Accesses]")
        for access in result.struct_accesses[:20]:  # Limit output
            print(f"  {access.direction}: {access.instruction}")
    
    # Issues and warnings
    if result.issues:
        print("\n[Issues]")
        for issue in result.issues:
            print(f"  ✗ {issue}")
    
    if result.warnings:
        print("\n[Warnings]")
        for warning in result.warnings:
            print(f"  ⚠ {warning}")
    
    print("\n" + "=" * 70)


def run_full_analysis(source_code: str, verbose: bool = False) -> AnalysisResult:
    """Run full analysis on source code."""
    # Compile to assembly
    asm, error = compile_to_asm(source_code)
    if error:
        result = AnalysisResult(source_file='error')
        result.issues.append(f"Compilation failed: {error}")
        return result
    
    # Analyze
    result = analyze_all_functions(asm)
    
    # Print results
    print_analysis(result, verbose)
    
    return result


def main():
    parser = argparse.ArgumentParser(description='Struct Assembly Analyzer for ZeroCC')
    parser.add_argument('--asm', '-a', help='Assembly file to analyze')
    parser.add_argument('--source', '-s', help='Source file to compile and analyze')
    parser.add_argument('--analyze-structs', action='store_true',
                        help='Focus on struct definitions')
    parser.add_argument('--analyze-calls', action='store_true',
                        help='Focus on function calls')
    parser.add_argument('--verbose', '-v', action='store_true',
                        help='Verbose output')
    parser.add_argument('--full-analysis', action='store_true',
                        help='Run full analysis with all checks')
    
    args = parser.parse_args()
    
    if args.asm:
        with open(args.asm, 'r') as f:
            asm = f.read()
        
        result = analyze_all_functions(asm)
        print_analysis(result, args.verbose)
    
    elif args.source:
        with open(args.source, 'r') as f:
            source_code = f.read()
        
        result = run_full_analysis(source_code, args.verbose)
    
    elif args.full_analysis:
        # Run with built-in test cases
        test_cases = [
            ("1_chunk_struct", """
typedef struct { int x; int y; } Pair;
Pair make_pair(int x, int y) { return (Pair){x, y}; }
int take_pair(Pair p) { return p.x + p.y; }
int main() { return take_pair(make_pair(1, 2)); }
"""),
            ("2_chunk_struct", """
typedef struct { int a; int b; int c; } Triple;
Triple make_triple(int a, int b, int c) { return (Triple){a, b, c}; }
int take_triple(Triple t) { return t.a + t.b + t.c; }
int main() { return take_triple(make_triple(1, 2, 3)); }
"""),
            ("3_chunk_struct", """
typedef struct { int a; int b; int c; int d; int e; } Pent;
Pent make_pent(int a, int b, int c, int d, int e) { 
    return (Pent){a, b, c, d, e}; 
}
int take_pent(Pent p) { return p.a + p.b + p.c + p.d + p.e; }
int main() { return take_pent(make_pent(1, 2, 3, 4, 5)); }
"""),
        ]
        
        for name, code in test_cases:
            print(f"\n--- {name} ---")
            run_full_analysis(code, args.verbose)
    
    else:
        # Default: run full analysis with built-in tests
        parser.print_help()
        sys.exit(1)


if __name__ == '__main__':
    main()
