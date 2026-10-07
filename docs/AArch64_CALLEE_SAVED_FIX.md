# AArch64 Callee-Saved Register Fix

## Problem

AArch64 callee-saved registers (x19-x28) are not being preserved across function calls.

### Root Causes

1. **Backend hooks are NULL** - `aarch64_conv.emit_callee_save_stores/loads = NULL`
2. **Register allocator doesn't track AArch64 callee-saved** - `used_callee_saved_gpr_mask` only tracks x86-64 registers

### Evidence

```bash
# Test returns wrong result (exit code 1 instead of 0)
qemu-aarch64-static /tmp/aarch64_callee_test
Exit code: 1
```

Disassembly shows no x19-x28 usage in current tests, but when they ARE used, they won't be preserved.

## Solution

### Step 1: Fix Register Allocator Mask Tracking

Make `used_callee_saved_gpr_mask` ISA-agnostic:

```c
// In regalloc.h - alloc_reg()
/* Track callee-saved register usage (ISA-specific) */
if (is_aarch64) {
    /* AArch64: x19-x28 are callee-saved */
    if (i >= 19 && i <= 28) {
        ra->used_callee_saved_gpr_mask |= (1u << i);
    }
} else {
    /* x86-64: rbx, r12-r15 are callee-saved */
    static const uint32_t x86_callee_saved_mask =
        (1u << 3) | (1u << 12) | (1u << 13) | (1u << 14) | (1u << 15);
    if ((x86_callee_saved_mask >> i) & 1u) {
        ra->used_callee_saved_gpr_mask |= (1u << i);
    }
}
```

### Step 2: Implement AArch64 Hooks

Add to `aarch64.c`:

```c
/* Callee-save slot offsets (AArch64 specific) */
static const int32_t aarch64_callee_save_slot_offset[] = {
    /* x19-x28 are callee-saved */
    8, 16, 24, 32, 40, 48, 56, 64, 72, 80  /* 10 registers */
};

/* Emit STORE for callee-saved registers */
static void aarch64_emit_callee_save_stores(ZeroCCCompiler *c) {
    uint32_t mask = c->regalloc.used_callee_saved_gpr_mask;
    while (mask) {
        int reg = __builtin_ctz(mask);
        if (reg >= 19 && reg <= 28) {
            int slot_idx = reg - 19;
            int32_t slot = aarch64_callee_save_slot_offset[slot_idx];
            if (BE(c)->emit_store) {
                BE(c)->emit_store(c, reg, BE(c)->fp_reg, -slot);
            }
        }
        mask &= mask - 1;
    }
}

/* Emit LOAD for callee-saved registers */
static void aarch64_emit_callee_save_loads(ZeroCCCompiler *c) {
    uint32_t mask = c->regalloc.used_callee_saved_gpr_mask;
    while (mask) {
        int reg = __builtin_ctz(mask);
        if (reg >= 19 && reg <= 28) {
            int slot_idx = reg - 19;
            int32_t slot = aarch64_callee_save_slot_offset[slot_idx];
            if (BE(c)->emit_load) {
                BE(c)->emit_load(c, reg, BE(c)->fp_reg, -slot);
            }
        }
        mask &= mask - 1;
    }
}
```

### Step 3: Update AArch64 Calling Convention

```c
static const ISACallingConvention aarch64_conv = {
    .int_arg_regs = aarch64_int_arg_regs,
    .int_arg_reg_count = 8,
    .fp_arg_regs = aarch64_fp_arg_regs,
    .fp_arg_reg_count = 8,
    .callee_saved_gpr = aarch64_callee_saved_gpr,
    .callee_saved_gpr_count = 12,
    .emit_callee_save_stores = aarch64_emit_callee_save_stores,
    .emit_callee_save_loads = aarch64_emit_callee_save_loads,
};
```

## Testing

```bash
# Compile and test
./build/bin/zerocc test/aarch64_callee_saved_test.c --target aarch64 -c -o /tmp/test.o
aarch64-linux-gnu-gcc /tmp/test.o -static -o /tmp/test
qemu-aarch64-static /tmp/test
echo "Exit code: $?"  # Should be 0
```

## Files to Modify

1. `src/zero_cc/parser/regalloc.h` - Fix mask tracking
2. `src/zero_cc/isa/aarch64.c` - Add implementation
3. `src/zero_cc/isa/aarch64.c` - Update calling convention

## Priority

**HIGH** - This is a critical bug that will cause incorrect code generation on AArch64 when callee-saved registers are used.
