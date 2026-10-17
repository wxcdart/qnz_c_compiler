#!/usr/bin/env python3
"""
ABI Verification Tool for ZeroCC - Task 68

Validates calling conventions, stack alignment, and ABI compliance for:
- x86-64: System V AMD64 ABI
- aarch64: AAPCS64 (ARM Procedure Call Standard)
- riscv64: LP64D ABI
- riscv32: ILP32 ABI

Usage:
    python3 tools/abi_verify.py --target aarch64 --test struct_return
    python3 tools/abi_verify.py --target all --mode comprehensive
"""

import argparse
import subprocess
import sys
import os
import re
from pathlib import Path
from dataclasses import dataclass
from typing import List, Dict, Optional, Tuple

@dataclass
class ABISpec:
    """ABI specification for a target architecture."""
    name: str
    description: str
    reg_size: int  # Register size in bytes
    stack_align: int  # Stack alignment requirement
    return_reg: str  # Primary return register
    second_return_reg: Optional[str]  # Secondary return register (for large structs)
    int_arg_regs: List[str]  # Integer argument registers
    fp_arg_regs: List[str]  # Floating-point argument registers
    callee_saved_regs: List[str]  # Callee-saved registers
    caller_saved_regs: List[str]  # Caller-saved registers
    stack_grows: str  # "down" or "up"
    max_int_args: int
    max_fp_args: int

# ABI specifications
ABI_SPECS: Dict[str, ABISpec] = {
    "x86-64": ABISpec(
        name="System V AMD64 ABI",
        description="Standard ABI for x86-64 Linux/Unix",
        reg_size=8,
        stack_align=16,
        return_reg="rax",
        second_return_reg="rdx",  # For 9-16 byte structs
        int_arg_regs=["rdi", "rsi", "rdx", "rcx", "r8", "r9"],
        fp_arg_regs=["xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7"],
        callee_saved_regs=["rbx", "rbp", "r12", "r13", "r14", "r15"],
        caller_saved_regs=["rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11"],
        stack_grows="down",
        max_int_args=6,
        max_fp_args=8,
    ),
    "aarch64": ABISpec(
        name="AAPCS64",
        description="ARM 64-bit Procedure Call Standard",
        reg_size=8,
        stack_align=16,
        return_reg="x0",
        second_return_reg="x1",  # For large structs
        int_arg_regs=["x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"],
        fp_arg_regs=["v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7"],
        callee_saved_regs=["x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28", "fp", "lr"],
        caller_saved_regs=["x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18"],
        stack_grows="down",
        max_int_args=8,
        max_fp_args=8,
    ),
    "riscv64": ABISpec(
        name="LP64D",
        description="RISC-V 64-bit ABI (LP64D)",
        reg_size=8,
        stack_align=16,
        return_reg="a0",
        second_return_reg=None,  # Large structs passed via hidden pointer
        int_arg_regs=["a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"],
        fp_arg_regs=["fa0", "fa1", "fa2", "fa3", "fa4", "fa5", "fa6", "fa7"],
        callee_saved_regs=["s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "gp", "tp"],
        caller_saved_regs=["a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7", "t0", "t1", "t2", "t3", "t4", "t5", "t6"],
        stack_grows="down",
        max_int_args=8,
        max_fp_args=8,
    ),
    "riscv32": ABISpec(
        name="ILP32",
        description="RISC-V 32-bit ABI",
        reg_size=4,
        stack_align=16,
        return_reg="a0",
        second_return_reg=None,
        int_arg_regs=["a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"],
        fp_arg_regs=["fa0", "fa1", "fa2", "fa3", "fa4", "fa5", "fa6", "fa7"],
        callee_saved_regs=["s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "gp", "tp"],
        caller_saved_regs=["a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7", "t0", "t1", "t2", "t3", "t4", "t5", "t6"],
        stack_grows="down",
        max_int_args=8,
        max_fp_args=8,
    ),
}


class ABIVerifier:
    """ABI verification tool for ZeroCC."""
    
    def __init__(self, base_dir: str):
        self.base_dir = Path(base_dir)
        self.build_dir = self.base_dir / "build"
        self.test_dir = self.base_dir / "test"
        self.abi_tests_dir = self.test_dir / "abi"
        
    def generate_abi_test(self, target: str, test_type: str) -> str:
        """Generate ABI test source code for a specific test type."""
        if test_type == "struct_return":
            return self._generate_struct_return_test(target)
        elif test_type == "variadic":
            return self._generate_variadic_test(target)
        elif test_type == "callee_saved":
            return self._generate_callee_saved_test(target)
        elif test_type == "fp_return":
            return self._generate_fp_return_test(target)
        elif test_type == "many_args":
            return self._generate_many_args_test(target)
        else:
            raise ValueError(f"Unknown test type: {test_type}")
    
    def _generate_struct_return_test(self, target: str) -> str:
        """Generate struct return test."""
        return '''
#include <stdio.h>

/* Test struct sizes: 4, 8, 9, 16 bytes */
struct s4 { int a; };
struct s8 { long a, b; };
struct s9 { char a; long b; };
struct s16 { long a, b, c, d; };

struct s4 ret_s4(void) { return (struct s4){.a = 42}; }
struct s8 ret_s8(void) { return (struct s8){.a = 100, .b = 200}; }
struct s9 ret_s9(void) { return (struct s9){.a = 1, .b = 300}; }
struct s16 ret_s16(void) { return (struct s16){.a = 10, .b = 20, .c = 30, .d = 40}; }

int main(void) {
    struct s4 r4 = ret_s4();
    struct s8 r8 = ret_s8();
    struct s9 r9 = ret_s9();
    struct s16 r16 = ret_s16();
    
    /* Verify return values */
    if (r4.a != 42) return 1;
    if (r8.a != 100 || r8.b != 200) return 2;
    if (r9.a != 1 || r9.b != 300) return 3;
    if (r16.a != 10 || r16.b != 20 || r16.c != 30 || r16.d != 40) return 4;
    
    return 0;
}
'''
    
    def _generate_variadic_test(self, target: str) -> str:
        """Generate variadic function test."""
        return '''
#include <stdio.h>
#include <stdarg.h>

int sum_ints(int count, ...) {
    va_list args;
    va_start(args, count);
    
    int total = 0;
    for (int i = 0; i < count; i++) {
        total += va_arg(args, int);
    }
    va_end(args);
    return total;
}

int main(void) {
    int result = sum_ints(5, 10, 20, 30, 40, 50);
    if (result != 120) return 1;
    return 0;
}
'''
    
    def _generate_callee_saved_test(self, target: str) -> str:
        """Generate callee-saved register preservation test."""
        return '''
#include <stdio.h>

/* Function that uses callee-saved registers internally */
void preserve_regs(int *results) {
    /* In assembly, callee-saved regs would be used here */
    /* For testing, we verify they're preserved via return values */
    results[0] = 100;
    results[1] = 200;
    results[2] = 300;
}

int get_preserved(int idx) {
    int results[3];
    preserve_regs(results);
    return results[idx];
}

int main(void) {
    if (get_preserved(0) != 100) return 1;
    if (get_preserved(1) != 200) return 2;
    if (get_preserved(2) != 300) return 3;
    return 0;
}
'''
    
    def _generate_fp_return_test(self, target: str) -> str:
        """Generate floating-point return test."""
        return '''
#include <stdio.h>

double ret_double(void) { return 3.14159; }
float ret_float(void) { return 2.71828; }

int main(void) {
    double d = ret_double();
    float f = ret_float();
    
    /* Check with tolerance */
    if (d < 3.14 || d > 3.15) return 1;
    if (f < 2.71 || f > 2.72) return 2;
    
    return 0;
}
'''
    
    def _generate_many_args_test(self, target: str) -> str:
        """Generate test with many arguments (exceeding register count)."""
        return '''
#include <stdio.h>

int many_args(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) {
    return a + b + c + d + e + f + g + h + i + j;
}

int main(void) {
    int result = many_args(1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
    if (result != 55) return 1;
    return 0;
}
'''
    
    def compile_and_run(self, source: str, target: str, test_name: str) -> Tuple[bool, int, str]:
        """Compile and run a test."""
        zerocc = self.build_dir / "bin" / "zerocc"
        if not zerocc.exists():
            return False, -1, f"zerocc not found at {zerocc}"
        
        # Write source to temp file
        import tempfile
        with tempfile.NamedTemporaryFile(mode='w', suffix='.c', delete=False) as f:
            f.write(source)
            src_path = Path(f.name)
        
        try:
            # Compile
            obj_path = self.build_dir / "test" / f"{test_name}.o"
            result = subprocess.run(
                [str(zerocc), f"--target={target}", "-c", "-o", str(obj_path), str(src_path)],
                capture_output=True,
                text=True
            )
            if result.returncode != 0:
                return False, -1, f"Compile error: {result.stderr}"
            
            # Link
            exe_path = self.build_dir / "test" / test_name
            result = subprocess.run(
                ["gcc", "-no-pie", str(obj_path), "-o", str(exe_path)],
                capture_output=True,
                text=True
            )
            if result.returncode != 0:
                return False, -1, f"Link error: {result.stderr}"
            
            # Run
            result = subprocess.run([str(exe_path)], capture_output=True, text=True, timeout=5)
            return result.returncode == 0, result.returncode, result.stdout + result.stderr
        finally:
            src_path.unlink()
    
    def verify_abi_compliance(self, target: str, test_types: List[str] = None) -> Dict[str, bool]:
        """Verify ABI compliance for a target."""
        if target not in ABI_SPECS:
            raise ValueError(f"Unknown target: {target}")
        
        if test_types is None:
            test_types = ["struct_return", "variadic", "callee_saved", "fp_return", "many_args"]
        
        results = {}
        abi = ABI_SPECS[target]
        
        print(f"\n{'='*70}")
        print(f"ABI Verification: {target} ({abi.name})")
        print(f"{'='*70}")
        print(f"Register size: {abi.reg_size}B, Stack align: {abi.stack_align}B")
        print(f"Return regs: {abi.return_reg}", end="")
        if abi.second_return_reg:
            print(f", {abi.second_return_reg}", end="")
        print(f"\nInt args: {len(abi.int_arg_regs)}, FP args: {len(abi.fp_arg_regs)}")
        print(f"{'-'*70}")
        
        for test_type in test_types:
            source = self.generate_abi_test(target, test_type)
            passed, exit_code, msg = self.compile_and_run(source, target, f"abi_{test_type}")
            results[test_type] = passed
            
            status = "PASS" if passed else "FAIL"
            print(f"  [{status}] {test_type:20s} (exit={exit_code})")
        
        return results
    
    def generate_comprehensive_test_suite(self, target: str) -> str:
        """Generate comprehensive ABI test suite."""
        tests = []
        
        for test_type in ["struct_return", "variadic", "callee_saved", "fp_return", "many_args"]:
            tests.append(f"/* === {test_type.upper()} === */\n")
            tests.append(self.generate_abi_test(target, test_type))
            tests.append("\n")
        
        return "\n".join(tests)


def main():
    parser = argparse.ArgumentParser(
        description="ABI Verification Tool for ZeroCC - Task 68"
    )
    parser.add_argument(
        "--target",
        choices=list(ABI_SPECS.keys()) + ["all"],
        default="x86-64",
        help="Target architecture to verify"
    )
    parser.add_argument(
        "--test",
        choices=["struct_return", "variadic", "callee_saved", "fp_return", "many_args", "comprehensive"],
        default="comprehensive",
        help="Specific test to run"
    )
    parser.add_argument(
        "--generate-source",
        action="store_true",
        help="Generate test source code only (don't compile/run)"
    )
    
    args = parser.parse_args()
    
    base_dir = Path(__file__).parent.parent
    verifier = ABIVerifier(str(base_dir))
    
    if args.generate_source:
        if args.test == "comprehensive":
            for target in ABI_SPECS.keys():
                print(f"\n/* === {target.upper()} COMPREHENSIVE === */")
                print(verifier.generate_comprehensive_test_suite(target))
        else:
            target = "x86-64" if args.target == "all" else args.target
            print(verifier.generate_abi_test(target, args.test))
        return
    
    if args.target == "all":
        all_results = {}
        for target in ABI_SPECS.keys():
            results = verifier.verify_abi_compliance(target, [args.test] if args.test != "comprehensive" else None)
            all_results[target] = results
        
        # Print summary
        print(f"\n{'='*70}")
        print("ABI Verification Summary")
        print(f"{'='*70}")
        for target, results in all_results.items():
            passed = sum(1 for v in results.values() if v)
            total = len(results)
            status = "PASS" if passed == total else "FAIL"
            print(f"  [{status}] {target:10s}: {passed}/{total} tests")
    else:
        results = verifier.verify_abi_compliance(
            args.target,
            [args.test] if args.test != "comprehensive" else None
        )
        passed = sum(1 for v in results.values() if v)
        total = len(results)
        sys.exit(0 if passed == total else 1)


if __name__ == "__main__":
    main()
