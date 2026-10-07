# Final Fix List: ISA-Agnostic Calling Convention

## Confirmed Issues (7 total)

| File | Line | Current | Fix | Impact |
|------|------|---------|-----|--------|
| `ast_lowering.c` | 95 | `xmm_idx < 8` | `BE(c)->conv->fp_arg_reg_count` | FP param save |
| `ast_lowering.c` | 98 | `int_idx < 6` | `BE(c)->conv->int_arg_reg_count` | Int param save |
| `ast_lowering_mc.c` | 386 | `arg_count > 6` | `BE(c)->conv->int_arg_reg_count` | Stack args |
| `ast_lowering_mc.c` | 390 | `a >= 6` | `BE(c)->conv->int_arg_reg_count` | Stack args loop |
| `ast_lowering_mc.c` | 403 | `arg_count < 6` / `: 6` | `BE(c)->conv->int_arg_reg_count` | Register args |
| `ast_lowering_mc.c` | 404 | `src_regs[6]` | `src_regs[BE(c)->conv->int_arg_reg_count]` | Array size |
| `ast_lowering_expr.c` | 1263 | `fp_reg_idx < 8` | `BE(c)->conv->fp_arg_reg_count` | FP arg spill |
| `ast_lowering_expr.c` | 1316 | `f < 8` | `BE(c)->conv->fp_arg_reg_count` | FP spill loop |

## False Positives (Not Issues)

| File | Line | Pattern | Why OK |
|------|------|---------|--------|
| `ast_lowering_mc.c` | 377 | `>> 8` | Bit shift, not comparison |
| `ast_lowering_expr.c` | 448 | `>> 8` | Bit shift, not comparison |
| `ast_lowering_expr.c` | 1018 | `>> 8` | Bit shift, not comparison |
| `ast_lowering_expr.c` | 1304 | `> 3` | Packed format (4 args max) |
| `ast_lowering_expr.c` | 1257 | `fp_spill_offsets[8]` | Should also be fixed |

## Additional Issue Found

| File | Line | Current | Fix |
|------|------|---------|-----|
| `ast_lowering_expr.c` | 1257 | `fp_spill_offsets[8]` | Use constant or dynamic size |

## Backend Tables (Already Correct)

```c
// x86_64.c
.int_arg_reg_count = 6,
.fp_arg_reg_count = 8,

// aarch64.c
.int_arg_reg_count = 8,
.fp_arg_reg_count = 8,

// riscv.c
.int_arg_reg_count = 8,
.fp_arg_reg_count = 8,
```

## Implementation Plan

### Step 1: Add a constant for max args (safe approach)

```c
// In ast_lowering_mc.c and ast_lowering_expr.c
#define MAX_INT_ARGS 8  // Maximum across all ISAs
#define MAX_FP_ARGS 8   // Maximum across all ISAs

// Then use:
int src_regs[MAX_INT_ARGS];
int fp_spill_offsets[MAX_FP_ARGS];
```

### Step 2: Replace hardcoded comparisons

```c
// ast_lowering.c
if (is_fp && xmm_idx < BE(c)->conv->fp_arg_reg_count) { ... }
else if (!is_fp && int_idx < BE(c)->conv->int_arg_reg_count) { ... }

// ast_lowering_mc.c
int int_arg_count = BE(c)->conv->int_arg_reg_count;
int n_stack = (arg_count > int_arg_count) ? arg_count - int_arg_count : 0;
for (int a = arg_count - 1; a >= int_arg_count; a--) { ... }
int n_args = (arg_count < int_arg_count) ? arg_count : int_arg_count;

// ast_lowering_expr.c
if (fp_reg_idx < BE(c)->conv->fp_arg_reg_count) { ... }
for (int f = 0; f < fp_reg_idx && f < BE(c)->conv->fp_arg_reg_count; f++) { ... }
```

## Testing

```bash
# Before changes
make test  # 118/118 pass

# After changes
make test  # 118/118 must still pass

# Verify AArch64 gets 8 args
python3 tools/isa_diff.py test/aot/arithmetic.c --isas x86_64 aarch64
```

## Risk Assessment

| Change | Risk | Notes |
|--------|------|-------|
| Array size changes | Low | Using constants, not dynamic allocation |
| Comparison changes | Low | Direct 1:1 replacements |
| Backend tables | None | Already correct |

**Total: 8 fixes needed across 3 files**
