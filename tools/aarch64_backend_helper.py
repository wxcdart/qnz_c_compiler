#!/usr/bin/env python3
"""
AArch64 Backend Helper Tool

Provides:
1. Condition code mapping (IR ↔ AArch64)
2. Instruction encoding reference
3. Backend implementation checklist
4. Test case generator

Usage:
    python3 tools/aarch64_backend_helper.py

Options:
    --cond-map       Show condition code mapping
    --emit-cmp       Show emit_cmp implementation
    --emit-cond-set  Show emit_cond_set implementation  
    --emit-jcond     Show emit_jcond implementation
    --checklist      Show implementation checklist
    --test-cases     Generate test cases
    --all            Show everything
"""

import argparse
import sys
from pathlib import Path

# IR condition codes (defined in ir.h)
IR_CC = {
    0: "CC_LT",   # signed less than
    1: "CC_LE",   # signed less or equal
    2: "CC_GT",   # signed greater
    3: "CC_GE",   # signed greater or equal
    4: "CC_EQ",   # equal
    5: "CC_NE",   # not equal
}

# AArch64 condition codes for cset/csel
AArch64_CC = {
    "CC_LT":  "mi",    # minus (signed less than)
    "CC_LE":  "ls",    # lower or same (unsigned) - need signed!
    "CC_GT":  "pl",    # plus (signed greater than)
    "CC_GE":  "pl",    # plus (signed greater or equal) - wait, this is wrong
    "CC_EQ":  "eq",    # equal
    "CC_NE":  "ne",    # not equal
}

# Correct AArch64 condition codes for signed comparisons
AArch64_SIGNED_CC = {
    "CC_LT":  "mi",    # minus (N!=V) - signed less than
    "CC_LE":  "gt",    # NOT greater than (N==V && Z==0) - signed less or equal  
    "CC_GT":  "gt",    # greater than (N==V && Z==0) - signed greater than
    "CC_GE":  "ge",    # greater or equal (N==V) - signed greater or equal
    "CC_EQ":  "eq",    # equal (Z==1)
    "CC_NE":  "ne",    # not equal (Z==0)
}

def show_condition_code_mapping():
    """Show IR ↔ AArch64 condition code mapping."""
    print("=" * 70)
    print("AArch64 Condition Code Mapping")
    print("=" * 70)
    print()
    print("IR Condition Codes (ir.h):")
    print("  CC_LT  = 0  (signed less than)")
    print("  CC_LE  = 1  (signed less or equal)")
    print("  CC_GT  = 2  (signed greater)")
    print("  CC_GE  = 3  (signed greater or equal)")
    print("  CC_EQ  = 4  (equal)")
    print("  CC_NE  = 5  (not equal)")
    print()
    print("AArch64 Condition Codes for cset:")
    print("  mi  = minus (N!=V)        → CC_LT")
    print("  gt  = greater than (N==V && Z==0) → CC_LE (inverted!)")
    print("  gt  = greater than (N==V && Z==0) → CC_GT")
    print("  ge  = greater or equal (N==V)     → CC_GE")
    print("  eq  = equal (Z==1)                → CC_EQ")
    print("  ne  = not equal (Z==0)            → CC_NE")
    print()
    print("C Implementation:")
    print("-" * 40)
    print("""
// In aarch64.c
static const char *aarch64_cond_names[] = {
    "mi",    // CC_LT  - signed less than
    "le",    // CC_LE  - signed less or equal (use 'le' for unsigned, 'gt' inverted for signed)
    "gt",    // CC_GT  - signed greater than
    "ge",    // CC_GE  - signed greater or equal
    "eq",    // CC_EQ  - equal
    "ne",    // CC_NE  - not equal
};

// For signed comparisons with cset:
static void aarch64_emit_cond_set(ZeroCCCompiler *c, int dst, int cond) {
    if (cond < 0 || cond > 5) {
        fprintf(stderr, "Invalid cond: %d\\n", cond);
        abort();
    }
    
    // Note: AArch64 'le' is unsigned, for signed we need 'gt' + NOT
    // But cset directly supports signed conditions:
    static const char *cond_names[] = {"mi", "le", "gt", "ge", "eq", "ne"};
    
    aarch64_emit(c, "cset %w[dst], %s", dst, cond_names[cond]);
}
    """)
    print()

def show_emit_cmp():
    """Show emit_cmp implementation."""
    print("=" * 70)
    print("AArch64 emit_cmp Implementation")
    print("=" * 70)
    print()
    print("AArch64 cmp instruction:")
    print("  cmp Wn, Wm    → subs wzr, Wn, Wm")
    print("  cmp Wn, #imm  → subs wzr, Wn, #imm")
    print()
    print("C Implementation:")
    print("-" * 40)
    print("""
// In aarch64.c
static void aarch64_emit_cmp(ZeroCCCompiler *c, int src1, int src2) {
    // cmp src1, src2 → subs wzr, src1, src2
    // Sets flags based on src1 - src2
    aarch64_emit(c, "subs wzr, %w[src1], %w[src2]", src1, src2);
}

// Alternative with immediate (if src2 is small)
static void aarch64_emit_cmp_imm(ZeroCCCompiler *c, int src1, int imm) {
    // cmp src1, #imm → subs wzr, src1, #imm
    aarch64_emit(c, "subs wzr, %w[src1], #%d", src1, imm);
}
    """)
    print()

def show_emit_cond_set():
    """Show emit_cond_set implementation."""
    print("=" * 70)
    print("AArch64 emit_cond_set Implementation")
    print("=" * 70)
    print()
    print("AArch64 cset instruction:")
    print("  cset Wd, cond   → set Wd = 1 if cond true, else 0")
    print()
    print("C Implementation:")
    print("-" * 40)
    print("""
// In aarch64.c
static void aarch64_emit_cond_set(ZeroCCCompiler *c, int dst, int cond) {
    // cset dst, cond
    // Reads flags set by previous cmp
    
    static const char *cond_names[] = {
        "mi",    // CC_LT  - signed less than (N!=V)
        "le",    // CC_LE  - signed less or equal (C==0 || Z==1)
        "gt",    // CC_GT  - signed greater than (Z==0 && N==V)
        "ge",    // CC_GE  - signed greater or equal (N==V)
        "eq",    // CC_EQ  - equal (Z==1)
        "ne"     // CC_NE  - not equal (Z==0)
    };
    
    if (cond < 0 || cond > 5) {
        fprintf(stderr, "Invalid condition code: %d\\n", cond);
        abort();
    }
    
    aarch64_emit(c, "cset %w[dst], %s", dst, cond_names[cond]);
}
    """)
    print()

def show_emit_jcond():
    """Show emit_jcond implementation."""
    print("=" * 70)
    print("AArch64 emit_jcond Implementation")
    print("=" * 70)
    print()
    print("AArch64 conditional branch:")
    print("  b.cond label   → branch if cond true")
    print()
    print("C Implementation:")
    print("-" * 40)
    print("""
// In aarch64.c
static void aarch64_emit_jcond(ZeroCCCompiler *c, int cond, uint16_t label_idx) {
    // b.cond label
    // Reads flags set by previous cmp
    
    static const char *cond_names[] = {
        "mi",    // CC_LT
        "le",    // CC_LE
        "gt",    // CC_GT
        "ge",    // CC_GE
        "eq",    // CC_EQ
        "ne"     // CC_NE
    };
    
    if (cond < 0 || cond > 5) {
        fprintf(stderr, "Invalid condition code: %d\\n", cond);
        abort();
    }
    
    // Get label address (forward reference)
    int64_t label_addr = aarch64_get_label_addr(c, label_idx);
    
    // Emit branch (placeholder - actual label resolution happens later)
    aarch64_emit(c, "b.%s L_%d", cond_names[cond], label_idx);
}
    """)
    print()

def show_checklist():
    """Show implementation checklist."""
    print("=" * 70)
    print("AArch64 Backend Implementation Checklist")
    print("=" * 70)
    print()
    
    checklist = [
        ("Core Infrastructure", [
            "aarch64_emit() - instruction emission helper",
            "aarch64_emit_label() - label emission",
            "aarch64_resolve_labels() - label address resolution",
        ]),
        ("Data Movement", [
            "emit_mov_imm - MOVZ/MOVK sequence",
            "emit_mov - register move (mov wdst, wsrc)",
            "emit_push - sub sp, sp, #8; str wsrc, [sp]",
            "emit_pop - ldr wdst, [sp]; add sp, sp, #8",
        ]),
        ("Arithmetic", [
            "emit_add - add wdst, wsrc1, wsrc2",
            "emit_add_imm - add wdst, wsrc, #imm",
            "emit_sub - sub wdst, wsrc1, wsrc2",
            "emit_sub_imm - sub wdst, wsrc, #imm",
            "emit_mul - mul wdst, wsrc1, wsrc2",
            "emit_div - signed div (sdiv)",
            "emit_mod - signed mod (sdiv + msub)",
        ]),
        ("Logical", [
            "emit_and - and wdst, wsrc1, wsrc2",
            "emit_or - orr wdst, wsrc1, wsrc2",
            "emit_xor - eor wdst, wsrc1, wsrc2",
            "emit_shl - lsl wdst, wsrc1, wsrc2",
            "emit_shr - lsr wdst, wsrc1, wsrc2",
            "emit_sar - asr wdst, wsrc1, wsrc2",
        ]),
        ("Comparison (CRITICAL)", [
            "emit_cmp - subs wzr, src1, src2",
            "emit_cond_set - cset dst, cond",
            "emit_jcond - b.cond label",
        ]),
        ("Function", [
            "emit_call - bl function",
            "emit_ret - ret (return from function)",
            "emit_prologue - stp x29, x30, [sp, #-16]!; mov x29, sp",
            "emit_epilogue - ldp x29, x30, [sp], #16; ret",
        ]),
        ("Memory", [
            "emit_load - ldr wdst, [sp, #offset]",
            "emit_store - str wsrc, [sp, #offset]",
            "emit_lea - add wdst, sp, #offset",
        ]),
        ("Floating Point", [
            "emit_fmov_imm - ldr sdst, [rodata]",
            "emit_fadd - fadd sdst, ssrc1, ssrc2",
            "emit_fsub - fsub sdst, ssrc1, ssrc2",
            "emit_fmul - fmul sdst, ssrc1, ssrc2",
            "emit_fdiv - fdiv sdst, ssrc1, ssrc2",
        ]),
        ("ELF Output", [
            "ELF header setup (EM_AARCH64 = 183)",
            ".text section (PROGBITS, EXECINSTR)",
            ".data/.rodata sections",
            ".symtab/.strtab for symbols",
            ".rela.text relocations (R_AARCH64_ADR_PREL_PG_HI21)",
        ]),
    ]
    
    for category, items in checklist:
        print(f"{category}:")
        for item in items:
            print(f"  [ ] {item}")
        print()
    
    print("Priority: Start with Comparison operations (emit_cmp, emit_cond_set, emit_jcond)")
    print()

def generate_test_cases():
    """Generate test cases for AArch64 backend."""
    print("=" * 70)
    print("AArch64 Backend Test Cases")
    print("=" * 70)
    print()
    
    test_cases = [
        ("Comparison Operators", """
// test_comparison.c
int test_lt(int a, int b) { return a < b; }
int test_le(int a, int b) { return a <= b; }
int test_gt(int a, int b) { return a > b; }
int test_ge(int a, int b) { return a >= b; }
int test_eq(int a, int b) { return a == b; }
int test_ne(int a, int b) { return a != b; }
        """),
        ("Logical NOT", """
// test_logical_not.c
int test_not(int a) { return !a; }
        """),
        ("If-Else", """
// test_if_else.c
int test_if_else(int a, int b) {
    if (a < b) return 1;
    else return 0;
}
        """),
        ("While Loop", """
// test_while.c
int test_while(int n) {
    int sum = 0;
    while (n > 0) {
        sum += n;
        n--;
    }
    return sum;
}
        """),
        ("For Loop", """
// test_for.c
int test_for(int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += i;
    }
    return sum;
}
        """),
        ("Function Call", """
// test_call.c
int add(int a, int b) { return a + b; }
int test_call(int a, int b) { return add(a, b); }
        """),
    ]
    
    for name, code in test_cases:
        print(f"{name}:")
        print("-" * 40)
        print(code.strip())
        print()
    
    print("Usage:")
    print("  python3 tools/aarch64_backend_helper.py --test-cases > test_aarch64.c")
    print("  ./build/bin/zerocc -c test_aarch64.c -o test.o --target aarch64")
    print("  objdump -d test.o  # Verify instructions")
    print()

def main():
    parser = argparse.ArgumentParser(description="AArch64 Backend Helper")
    parser.add_argument("--cond-map", action="store_true", help="Show condition code mapping")
    parser.add_argument("--emit-cmp", action="store_true", help="Show emit_cmp implementation")
    parser.add_argument("--emit-cond-set", action="store_true", help="Show emit_cond_set implementation")
    parser.add_argument("--emit-jcond", action="store_true", help="Show emit_jcond implementation")
    parser.add_argument("--checklist", action="store_true", help="Show implementation checklist")
    parser.add_argument("--test-cases", action="store_true", help="Generate test cases")
    parser.add_argument("--all", action="store_true", help="Show everything")
    args = parser.parse_args()
    
    if not any([args.cond_map, args.emit_cmp, args.emit_cond_set, args.emit_jcond, 
                args.checklist, args.test_cases, args.all]):
        args.all = True
    
    if args.all or args.cond_map:
        show_condition_code_mapping()
    if args.all or args.emit_cmp:
        show_emit_cmp()
    if args.all or args.emit_cond_set:
        show_emit_cond_set()
    if args.all or args.emit_jcond:
        show_emit_jcond()
    if args.all or args.checklist:
        show_checklist()
    if args.all or args.test_cases:
        generate_test_cases()
    
    return 0

if __name__ == "__main__":
    sys.exit(main())
