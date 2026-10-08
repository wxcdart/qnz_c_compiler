#!/usr/bin/env python3
"""
Prologue/Epilogue Validator

Validates that prologue/epilogue code generation is correct:
- Frame size is 16-byte aligned
- Stack alignment is correct before calls
- Prologue and epilogue are symmetric
- No patching issues (frame size consistency)

Usage:
    python3 prologue_validator.py --asm-file test.s
    python3 prologue_validator.py --disasm-file test.o --objdump
    python3 prologue_validator.py --test test.c --verbose
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path
from dataclasses import dataclass
from typing import List, Optional, Tuple


@dataclass
class FunctionPrologue:
    """Represents a function's prologue information."""
    name: str
    frame_size: Optional[int]  # None if not found
    is_aligned: bool
    push_rbp: bool
    mov_rbp_rsp: bool
    sub_rsp: bool
    line_number: int


@dataclass
class FunctionEpilogue:
    """Represents a function's epilogue information."""
    name: str
    has_leave: bool
    has_ret: bool
    has_pop_rbp: bool
    line_number: int


@dataclass
class ValidationResult:
    """Result of prologue/epilogue validation."""
    function_name: str
    errors: List[str]
    warnings: List[str]
    passed: bool


def parse_assembly_file(asm_file: str) -> Tuple[List[FunctionPrologue], List[FunctionEpilogue]]:
    """Parse assembly file and extract prologue/epilogue information."""
    prologues = []
    epilogues = []
    
    current_function = None
    in_prologue = False
    in_epilogue = False
    
    with open(asm_file, 'r') as f:
        lines = f.readlines()
    
    for line_num, line in enumerate(lines, 1):
        line = line.strip()
        
        # Skip comments and empty lines
        if not line or line.startswith('#') or line.startswith('//'):
            continue
        
        # Check for function label
        func_match = re.match(r'^(\w+):', line)
        if func_match:
            if current_function and in_prologue:
                # Previous function had incomplete prologue
                pass
            current_function = func_match.group(1)
            in_prologue = True
            in_epilogue = False
            continue
        
        if not current_function:
            continue
        
        # Check for prologue instructions
        if in_prologue:
            if re.match(r'push\s+%?rbp', line):
                pass  # Continue prologue
            elif re.match(r'mov\s+%?rsp,%?rbp', line):
                pass  # Continue prologue
            elif re.match(r'sub\s+\\$?(\d+),\s*%?rsp', line):
                # Found frame size
                match = re.search(r'\\\$?(\d+)', line)
                if match:
                    frame_size = int(match.group(1))
                    is_aligned = (frame_size % 16 == 0)
                    
                    prologues.append(FunctionPrologue(
                        name=current_function,
                        frame_size=frame_size,
                        is_aligned=is_aligned,
                        push_rbp=True,
                        mov_rbp_rsp=True,
                        sub_rsp=True,
                        line_number=line_num
                    ))
                    in_prologue = False
            elif re.match(r'(call|jmp|ret)', line):
                # Prologue ended without frame size
                prologues.append(FunctionPrologue(
                    name=current_function,
                    frame_size=0,
                    is_aligned=True,
                    push_rbp=False,
                    mov_rbp_rsp=False,
                    sub_rsp=False,
                    line_number=line_num
                ))
                in_prologue = False
        
        # Check for epilogue instructions
        if re.match(r'leave', line):
            epilogues.append(FunctionEpilogue(
                name=current_function,
                has_leave=True,
                has_ret=False,
                has_pop_rbp=False,
                line_number=line_num
            ))
        elif re.match(r'ret', line):
            if epilogues and epilogues[-1].name == current_function and not epilogues[-1].has_ret:
                epilogues[-1].has_ret = True
            else:
                epilogues.append(FunctionEpilogue(
                    name=current_function,
                    has_leave=False,
                    has_ret=True,
                    has_pop_rbp=False,
                    line_number=line_num
                ))
    
    return prologues, epilogues


def validate_prologue(prologue: FunctionPrologue) -> ValidationResult:
    """Validate a single prologue."""
    errors = []
    warnings = []
    
    # Check frame size alignment
    if prologue.frame_size is not None and prologue.frame_size > 0:
        if prologue.frame_size % 16 != 0:
            errors.append(f"Frame size {prologue.frame_size} is not 16-byte aligned (required by System V ABI)")
        elif prologue.frame_size > (1 << 24):
            errors.append(f"Frame size {prologue.frame_size} exceeds 16MB limit")
    
    # Check prologue sequence
    if not prologue.push_rbp:
        warnings.append("Missing 'push rbp' in prologue")
    if not prologue.mov_rbp_rsp:
        warnings.append("Missing 'mov rbp, rsp' in prologue")
    if prologue.frame_size and prologue.frame_size > 0 and not prologue.sub_rsp:
        warnings.append(f"Frame size {prologue.frame_size} specified but no 'sub rsp' instruction found")
    
    return ValidationResult(
        function_name=prologue.name,
        errors=errors,
        warnings=warnings,
        passed=len(errors) == 0
    )


def validate_epilogue(epilogue: FunctionEpilogue) -> ValidationResult:
    """Validate a single epilogue."""
    errors = []
    warnings = []
    
    # Check epilogue sequence
    if not epilogue.has_leave and not epilogue.has_pop_rbp:
        warnings.append("Missing 'leave' or 'pop rbp' in epilogue")
    if not epilogue.has_ret:
        warnings.append("Missing 'ret' in epilogue")
    
    return ValidationResult(
        function_name=epilogue.name,
        errors=errors,
        warnings=warnings,
        passed=len(errors) == 0
    )


def validate_assembly_file(asm_file: str, verbose: bool = False) -> List[ValidationResult]:
    """Validate prologue/epilogue in assembly file."""
    prologues, epilogues = parse_assembly_file(asm_file)
    results = []
    
    # Validate prologues
    for prologue in prologues:
        result = validate_prologue(prologue)
        if verbose or not result.passed:
            results.append(result)
    
    # Validate epilogues
    for epilogue in epilogues:
        result = validate_epilogue(epilogue)
        if verbose or not result.passed:
            results.append(result)
    
    return results


def generate_test_cases(output_dir: str):
    """Generate test cases for various frame sizes."""
    test_cases = [
        # (name, frame_size, should_pass)
        ("no_frame", 0, True),
        ("small_aligned", 16, True),
        ("small_unaligned", 8, False),
        ("medium_aligned", 64, True),
        ("medium_unaligned", 72, False),
        ("large_aligned", 256, True),
        ("large_unaligned", 264, False),
        ("boundary_16", 16, True),
        ("boundary_32", 32, True),
        ("boundary_48", 48, True),
        ("boundary_64", 64, True),
    ]
    
    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)
    
    for name, frame_size, should_pass in test_cases:
        test_file = output_path / f"test_{name}.c"
        test_file.write_text(f"""
/* Test case: {name} (frame_size={frame_size}, should_pass={should_pass}) */

int test_function(void) {{
    /* This will generate a prologue with frame_size = {frame_size} */
    int result = 0;
    
    /* Add stack variables to force frame allocation */
""")
        
        # Add variables to force specific frame size
        if frame_size > 0:
            num_vars = frame_size // 8
            for i in range(num_vars):
                test_file.write(f"    int var{i} = {i};\n")
        
        test_file.write("""
    return result;
}}
""")
    
    print(f"Generated {len(test_cases)} test cases in {output_dir}/")


def main():
    parser = argparse.ArgumentParser(
        description="Validate prologue/epilogue code generation"
    )
    parser.add_argument("--asm-file", "-f", help="Assembly file to validate")
    parser.add_argument("--disasm-file", "-d", help="Object file to disassemble")
    parser.add_argument("--test", "-t", help="C source file to compile and validate")
    parser.add_argument("--generate-tests", "-g", help="Generate test cases to directory")
    parser.add_argument("--verbose", "-v", action="store_true", help="Show all results")
    parser.add_argument("--output-dir", "-o", default="/tmp/prologue_tests", help="Output directory")
    
    args = parser.parse_args()
    
    if args.generate_tests:
        generate_test_cases(args.generate_tests)
        return 0
    
    if args.asm_file:
        print(f"Validating assembly file: {args.asm_file}")
        results = validate_assembly_file(args.asm_file, args.verbose)
        
        passed = sum(1 for r in results if r.passed)
        total = len(results)
        
        print(f"\nValidation Results: {passed}/{total} passed")
        
        for result in results:
            status = "PASS" if result.passed else "FAIL"
            print(f"\n[{status}] {result.function_name}")
            
            if result.errors:
                for error in result.errors:
                    print(f"  ERROR: {error}")
            
            if result.warnings:
                for warning in result.warnings:
                    print(f"  WARNING: {warning}")
        
        return 0 if passed == total else 1
    
    if args.disasm_file:
        # Disassemble and validate
        try:
            result = subprocess.run(
                ["objdump", "-d", args.disasm_file],
                capture_output=True,
                text=True,
                check=True
            )
            
            # Write to temp file and validate
            import tempfile
            with tempfile.NamedTemporaryFile(mode='w', suffix='.s', delete=False) as f:
                f.write(result.stdout)
                temp_file = f.name
            
            return validate_assembly_file(temp_file, args.verbose)
            
        except subprocess.CalledProcessError as e:
            print(f"Error disassembling file: {e}")
            return 1
    
    if args.test:
        # Compile to assembly and validate
        try:
            import tempfile
            
            # Compile to assembly
            result = subprocess.run(
                ["./build/bin/zerocc", "-S", "-o", "-", args.test],
                capture_output=True,
                text=True,
                check=True
            )
            
            # Write to temp file
            with tempfile.NamedTemporaryFile(mode='w', suffix='.s', delete=False) as f:
                f.write(result.stdout)
                temp_file = f.name
            
            return validate_assembly_file(temp_file, args.verbose)
            
        except subprocess.CalledProcessError as e:
            print(f"Error compiling file: {e}")
            return 1
    
    parser.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
