#!/usr/bin/env python3
"""
Backend Test Harness for ZeroCC - Task 67

Generates minimal test cases for each backend function.
Allows incremental testing as functions are implemented.

Usage:
    python3 tools/backend_test_harness.py --isa aarch64 --function emit_mov_reg
    python3 tools/backend_test_harness.py --isa aarch64 --generate-all
"""

import argparse
from pathlib import Path
from typing import Dict, List

# Test case templates for each backend function
TEST_TEMPLATES = {
    "core_emission": {
        "emit_mov_reg": """
/* Test: Register-to-register move */
int main(void) {
    int x = 42;
    int y = x;  /* Should move x to y */
    return y;   /* Expected: 42 */
}
""",
        "emit_add": """
/* Test: Integer addition */
int main(void) {
    return 10 + 20;  /* Expected: 30 */
}
""",
        "emit_sub": """
/* Test: Integer subtraction */
int main(void) {
    return 50 - 20;  /* Expected: 30 */
}
""",
        "emit_mul": """
/* Test: Integer multiplication */
int main(void) {
    return 6 * 7;  /* Expected: 42 */
}
""",
        "emit_div": """
/* Test: Integer division */
int main(void) {
    return 100 / 4;  /* Expected: 25 */
}
""",
        "emit_mod": """
/* Test: Integer modulo */
int main(void) {
    return 17 % 5;  /* Expected: 2 */
}
""",
        "emit_neg": """
/* Test: Integer negation */
int main(void) {
    int x = 42;
    return -x;  /* Expected: -42 */
}
""",
        "emit_and": """
/* Test: Bitwise AND */
int main(void) {
    return 0xFF & 0x0F;  /* Expected: 0x0F (15) */
}
""",
        "emit_or": """
/* Test: Bitwise OR */
int main(void) {
    return 0x0F | 0xF0;  /* Expected: 0xFF (255) */
}
""",
        "emit_xor": """
/* Test: Bitwise XOR */
int main(void) {
    return 0xFF ^ 0x0F;  /* Expected: 0xF0 (240) */
}
""",
        "emit_not": """
/* Test: Bitwise NOT */
int main(void) {
    return ~0x0F;  /* Expected: -16 (all bits inverted) */
}
""",
        "emit_shl": """
/* Test: Shift left */
int main(void) {
    return 1 << 4;  /* Expected: 16 */
}
""",
        "emit_shr": """
/* Test: Shift right */
int main(void) {
    return 16 >> 2;  /* Expected: 4 */
}
""",
    },
    "memory_ops": {
        "emit_deref": """
/* Test: Pointer dereference */
int main(void) {
    int x = 42;
    int *p = &x;
    return *p;  /* Expected: 42 */
}
""",
        "emit_load": """
/* Test: 64-bit load */
int main(void) {
    long x = 123456789;
    return x;  /* Expected: 123456789 */
}
""",
        "emit_load_32": """
/* Test: 32-bit load */
int main(void) {
    int x = 12345678;
    return x;  /* Expected: 12345678 */
}
""",
        "emit_load_16": """
/* Test: 16-bit load */
int main(void) {
    short x = 32767;
    return x;  /* Expected: 32767 */
}
""",
        "emit_load_byte": """
/* Test: 8-bit load */
int main(void) {
    char x = 100;
    return x;  /* Expected: 100 */
}
""",
        "emit_store": """
/* Test: 64-bit store */
int main(void) {
    long x;
    x = 999999999;
    return x;  /* Expected: 999999999 */
}
""",
        "emit_store_32": """
/* Test: 32-bit store */
int main(void) {
    int x;
    x = 8888888;
    return x;  /* Expected: 8888888 */
}
""",
        "emit_store_16": """
/* Test: 16-bit store */
int main(void) {
    short x;
    x = 30000;
    return x;  /* Expected: 30000 */
}
""",
        "emit_store_byte": """
/* Test: 8-bit store */
int main(void) {
    char x;
    x = 200;
    return x;  /* Expected: 200 */
}
""",
        "emit_lea": """
/* Test: Load effective address (address-of) */
int main(void) {
    int x = 42;
    int *p = &x;
    return *p;  /* Expected: 42 */
}
""",
        "emit_lea_rip": """
/* Test: RIP-relative addressing (globals) */
int global_var = 12345;
int main(void) {
    return global_var;  /* Expected: 12345 */
}
""",
    },
    "comparisons": {
        "emit_cmp": """
/* Test: Compare */
int main(void) {
    int a = 10, b = 20;
    return (a < b) ? 1 : 0;  /* Expected: 1 */
}
""",
        "emit_setl": """
/* Test: Set if less */
int main(void) {
    return (5 < 10) ? 1 : 0;  /* Expected: 1 */
}
""",
        "emit_setle": """
/* Test: Set if less or equal */
int main(void) {
    return (10 <= 10) ? 1 : 0;  /* Expected: 1 */
}
""",
        "emit_setg": """
/* Test: Set if greater */
int main(void) {
    return (20 > 10) ? 1 : 0;  /* Expected: 1 */
}
""",
        "emit_setge": """
/* Test: Set if greater or equal */
int main(void) {
    return (10 >= 10) ? 1 : 0;  /* Expected: 1 */
}
""",
        "emit_sete": """
/* Test: Set if equal */
int main(void) {
    return (10 == 10) ? 1 : 0;  /* Expected: 1 */
}
""",
        "emit_setne": """
/* Test: Set if not equal */
int main(void) {
    return (10 != 5) ? 1 : 0;  /* Expected: 1 */
}
""",
        "emit_cmp_zero": """
/* Test: Compare to zero */
int main(void) {
    int x = 0;
    return (x == 0) ? 1 : 0;  /* Expected: 1 */
}
""",
        "emit_lnot": """
/* Test: Logical NOT */
int main(void) {
    return !0;  /* Expected: 1 */
}
""",
    },
    "control_flow": {
        "emit_label": """
/* Test: Labels (goto) */
int main(void) {
    goto end;
    return 1;
end:
    return 0;  /* Expected: 0 */
}
""",
        "emit_jmp": """
/* Test: Unconditional jump */
int main(void) {
    goto skip;
    return 1;
skip:
    return 0;  /* Expected: 0 */
}
""",
        "emit_jmp_cond": """
/* Test: Conditional jump */
int main(void) {
    if (1) {
        return 42;  /* Expected: 42 */
    }
    return 0;
}
""",
        "emit_call": """
/* Test: Function call */
int add(int a, int b) {
    return a + b;
}
int main(void) {
    return add(10, 20);  /* Expected: 30 */
}
""",
        "emit_ret": """
/* Test: Return */
int main(void) {
    return 42;  /* Expected: 42 */
}
""",
    },
    "function_abi": {
        "emit_prologue": """
/* Test: Function prologue (multiple functions) */
int func1(void) { return 1; }
int func2(void) { return 2; }
int main(void) {
    return func1() + func2();  /* Expected: 3 */
}
""",
        "emit_epilogue": """
/* Test: Function epilogue (return values) */
int main(void) {
    return 42;  /* Expected: 42 */
}
""",
        "emit_param_save_int": """
/* Test: Integer parameters */
int sum(int a, int b, int c) {
    return a + b + c;
}
int main(void) {
    return sum(10, 20, 30);  /* Expected: 60 */
}
""",
        "emit_param_save_fp": """
/* Test: FP parameters */
double add(double a, double b) {
    return a + b;
}
int main(void) {
    double r = add(1.5, 2.5);
    return (r > 3.9 && r < 4.1) ? 0 : 1;  /* Expected: 0 */
}
""",
        "emit_variadic_spill": """
/* Test: Variadic functions */
#include <stdarg.h>
int sum(int n, ...) {
    va_list args;
    va_start(args, n);
    int total = 0;
    for (int i = 0; i < n; i++) {
        total += va_arg(args, int);
    }
    va_end(args);
    return total;
}
int main(void) {
    return sum(3, 10, 20, 30);  /* Expected: 60 */
}
""",
        "emit_variadic_al_setup": """
/* Test: Variadic setup */
#include <stdarg.h>
int first(int n, ...) {
    va_list args;
    va_start(args, n);
    int r = va_arg(args, int);
    va_end(args);
    return r;
}
int main(void) {
    return first(1, 42);  /* Expected: 42 */
}
""",
    },
    "floating_point": {
        "emit_fload": """
/* Test: FP load */
int main(void) {
    double x = 3.14;
    return (x > 3.1 && x < 3.2) ? 0 : 1;  /* Expected: 0 */
}
""",
        "emit_fstore": """
/* Test: FP store */
int main(void) {
    double x;
    x = 2.71;
    return (x > 2.7 && x < 2.8) ? 0 : 1;  /* Expected: 0 */
}
""",
        "emit_fadd": """
/* Test: FP addition */
int main(void) {
    return (1.5 + 2.5 > 3.9 && 1.5 + 2.5 < 4.1) ? 0 : 1;  /* Expected: 0 */
}
""",
        "emit_fsub": """
/* Test: FP subtraction */
int main(void) {
    return (5.0 - 2.0 > 2.9 && 5.0 - 2.0 < 3.1) ? 0 : 1;  /* Expected: 0 */
}
""",
        "emit_fmul": """
/* Test: FP multiplication */
int main(void) {
    return (2.0 * 3.0 > 5.9 && 2.0 * 3.0 < 6.1) ? 0 : 1;  /* Expected: 0 */
}
""",
        "emit_fdiv": """
/* Test: FP division */
int main(void) {
    return (10.0 / 2.0 > 4.9 && 10.0 / 2.0 < 5.1) ? 0 : 1;  /* Expected: 0 */
}
""",
        "emit_fmov_imm": """
/* Test: FP move immediate */
int main(void) {
    double x = 1.0;
    return (x > 0.9 && x < 1.1) ? 0 : 1;  /* Expected: 0 */
}
""",
        "emit_fcmp": """
/* Test: FP comparison */
int main(void) {
    double a = 1.5, b = 2.5;
    return (a < b) ? 0 : 1;  /* Expected: 0 */
}
""",
    },
}


class BackendTestHarness:
    """Generate test cases for backend functions."""
    
    def __init__(self, base_dir: str):
        self.base_dir = Path(base_dir)
    
    def generate_test(self, isa: str, category: str, func_name: str) -> str:
        """Generate test for a specific function."""
        if category in TEST_TEMPLATES and func_name in TEST_TEMPLATES[category]:
            return TEST_TEMPLATES[category][func_name]
        return f"/* Test not available for {func_name} */\n"
    
    def generate_all_tests(self, isa: str) -> Dict[str, str]:
        """Generate all tests for an ISA."""
        tests = {}
        for category, funcs in TEST_TEMPLATES.items():
            for func_name, template in funcs.items():
                test_name = f"{isa}_{category}_{func_name}.c"
                tests[test_name] = template
        return tests
    
    def save_test(self, test_name: str, source: str, output_dir: str):
        """Save a test to file."""
        filepath = Path(output_dir) / test_name
        filepath.parent.mkdir(parents=True, exist_ok=True)
        with open(filepath, 'w') as f:
            f.write(source)
        return filepath


def main():
    parser = argparse.ArgumentParser(
        description="Backend Test Harness for ZeroCC - Task 67"
    )
    parser.add_argument(
        "--isa",
        choices=["x86_64", "aarch64", "riscv"],
        required=True,
        help="Target ISA"
    )
    parser.add_argument(
        "--function",
        help="Generate test for specific function"
    )
    parser.add_argument(
        "--category",
        choices=["core_emission", "memory_ops", "comparisons", "control_flow", "function_abi", "floating_point"],
        help="Category of function"
    )
    parser.add_argument(
        "--generate-all",
        action="store_true",
        help="Generate all tests"
    )
    parser.add_argument(
        "--output-dir",
        default="/tmp/backend_tests",
        help="Output directory"
    )
    
    args = parser.parse_args()
    
    harness = BackendTestHarness(str(Path(__file__).parent.parent))
    
    if args.generate_all:
        tests = harness.generate_all_tests(args.isa)
        output_dir = Path(args.output_dir)
        output_dir.mkdir(parents=True, exist_ok=True)
        
        for test_name, source in tests.items():
            path = harness.save_test(test_name, source, str(output_dir))
            print(f"  Generated: {path}")
        
        print(f"\nTotal: {len(tests)} tests generated")
    elif args.function and args.category:
        test = harness.generate_test(args.isa, args.category, args.function)
        print(test)
    else:
        print("Usage: --generate-all or --category <cat> --function <name>")


if __name__ == "__main__":
    main()
