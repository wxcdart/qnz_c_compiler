# Prologue/Epilogue Robustness Improvement Plan

## Overview

This document proposes a practical, phased approach to make prologue/epilogue handling more robust without breaking existing code.

## Current State (Fragile)

```
IR Emission (early)
    ↓ frame_size = placeholder
IR Patching (late, after regalloc)
    ↓ frame_size = overwritten
Machine Code Lowering
    ↓ uses instr->imm64
ISA Backend
    ↓ emits actual instructions
Assembly Emitter (separate path)
    ↓ also uses instr->imm64, but different code
```

**Problem**: 5 different code paths, no guarantee they agree on frame size

## Target State (Robust)

```
Frame Layout Calculation (single authority)
    ↓ calculates frame_size with validation
IR Creation
    ↓ prologue has correct frame_size from start
Register Allocation
    ↓ doesn't change frame_size
Machine Code Lowering
    ↓ reads frame_size from IR
ISA Backend
    ↓ emits instructions consistent with frame_size
Assembly Emitter
    ↓ emits same prologue structure
```

**Goal**: Single source of truth for frame size, validated once, used consistently

---

## Improvement Phases

### Phase 1: Add Validation (Immediate, 2-4 hours)

**Goal**: Catch bugs early before they cause segfaults

**Changes**:

1. Create `src/zero_cc/parser/prologue_epilogue.h`:

```c
#ifndef PROLOGUE_EPILOGUE_H
#define PROLOGUE_EPILOGUE_H

#include "ir.h"
#include "../types/symbols.h"

/* Validate prologue/epilogue IR before lowering */
int prologue_epilogue_validate(ZeroCCCompiler *c) {
    Instr *prologue = ir_get(0);
    
    /* Check prologue exists and is first instruction */
    if (!prologue || prologue->op != OP_PROLOGUE) {
        fprintf(stderr, "ERROR: First IR instruction is not OP_PROLOGUE\n");
        return 0;
    }
    
    /* Validate frame size */
    size_t frame_size = (size_t)prologue->imm64;
    
    /* Frame size must be 16-byte aligned */
    if (frame_size % 16 != 0) {
        fprintf(stderr, "ERROR: Frame size %zu not 16-byte aligned (required by ABI)\n", 
                frame_size);
        return 0;
    }
    
    /* Frame size must be reasonable (< 16MB) */
    if (frame_size > (1u << 24)) {
        fprintf(stderr, "ERROR: Frame size %zu exceeds 16MB limit\n", frame_size);
        return 0;
    }
    
    /* Check epilogue exists (for symmetry) */
    Instr *epilogue = NULL;
    for (size_t i = ir_get_count() - 1; i < ir_get_count(); i--) {
        if (ir_get(i)->op == OP_EPILOGUE) {
            epilogue = ir_get(i);
            break;
        }
    }
    
    if (!epilogue) {
        fprintf(stderr, "WARNING: No OP_EPILOGUE found in IR\n");
        /* Not fatal - epilogue might not be needed for some functions */
    }
    
    return 1;
}

#endif
```

2. Call validation in `ast_lowering.c` before lowering:

```c
void zero_cc_lower_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef root) {
    /* ... existing code ... */
    
    /* Validate prologue/epilogue before lowering to machine code */
    if (!prologue_epilogue_validate(c)) {
        fprintf(stderr, "FATAL: Prologue/epilogue validation failed\n");
        return;  /* or exit(1) */
    }
    
    /* ... continue with lowering ... */
}
```

**Risk**: Very low - just adds validation, doesn't change behavior

**Benefit**: Catches misconfigurations early with clear error messages

---

### Phase 2: Centralize Frame Size Calculation (2-3 days)

**Goal**: Single source of truth for frame size, before IR is emitted

**Changes**:

1. Create `src/zero_cc/parser/frame_layout.c`:

```c
#include "frame_layout.h"
#include "regalloc.h"

/* Frame layout structure combines all frame components */
typedef struct {
    size_t callee_save_bytes;    /* Space for callee-saved regs */
    size_t spill_bytes;          /* Space for spilled temps */
    size_t param_save_bytes;     /* Space for incoming parameters */
    size_t total_frame_size;     /* Total aligned frame size */
    int32_t spill_base_offset;   /* Where spill slots start */
} FrameLayout;

/* Calculate frame layout (single source of truth) */
FrameLayout frame_layout_calculate(
    uint32_t callee_save_mask,
    size_t current_stack_frame,
    int next_spill_slot
) {
    FrameLayout layout = {0};
    
    /* Callee-saved register area: fixed at top of frame */
    layout.callee_save_bytes = 40;  /* 5 registers × 8 bytes */
    
    /* Spill area: below callee-saved area */
    if (current_stack_frame < CALLEE_SAVE_FRAME_RESERVE) {
        layout.spill_base_offset = CALLEE_SAVE_FRAME_RESERVE;
    } else {
        layout.spill_base_offset = current_stack_frame;
    }
    
    /* Additional spill slots */
    layout.spill_bytes = next_spill_slot * 8;
    
    /* Total frame size, 16-byte aligned */
    size_t total = layout.spill_base_offset + layout.spill_bytes;
    layout.total_frame_size = (total + 15) & ~15;
    
    /* Validate alignment */
    assert(layout.total_frame_size % 16 == 0, 
           "Frame size not 16-byte aligned: %zu", layout.total_frame_size);
    
    return layout;
}

/* Format frame layout for debugging */
void frame_layout_dump(const FrameLayout *layout, FILE *out) {
    fprintf(out, "Frame Layout:\n");
    fprintf(out, "  Callee-save area: %zu bytes\n", layout->callee_save_bytes);
    fprintf(out, "  Spill base offset: %d\n", layout->spill_base_offset);
    fprintf(out, "  Spill slots: %zu bytes\n", layout->spill_bytes);
    fprintf(out, "  Total frame size: %zu bytes (16-byte aligned)\n", 
            layout->total_frame_size);
}
```

2. Create header `src/zero_cc/parser/frame_layout.h`:

```c
#ifndef FRAME_LAYOUT_H
#define FRAME_LAYOUT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    size_t callee_save_bytes;
    size_t spill_bytes;
    size_t param_save_bytes;
    size_t total_frame_size;
    int32_t spill_base_offset;
} FrameLayout;

FrameLayout frame_layout_calculate(
    uint32_t callee_save_mask,
    size_t current_stack_frame,
    int next_spill_slot
);

void frame_layout_dump(const FrameLayout *layout, FILE *out);

#endif
```

3. Use in `ast_lowering.c`:

```c
#include "frame_layout.h"

void zero_cc_lower_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef root) {
    /* ... register allocation ... */
    
    /* Calculate frame layout (single source of truth) */
    FrameLayout layout = frame_layout_calculate(
        c->regalloc.used_callee_saved_gpr_mask,
        c->current_stack_frame,
        c->regalloc.next_spill_slot
    );
    
    /* Emit prologue with correct frame size */
    ir_emit_prologue(layout.total_frame_size);
    
    /* ... rest of IR emission ... */
}
```

**Risk**: Medium - changes frame size calculation order, but logic is identical

**Benefit**: Frame size calculated once, validated immediately, used everywhere

---

### Phase 3: Validate Frame Size Consistency (1 day)

**Goal**: Ensure all code paths use same frame size

**Changes**:

1. Add to `prologue_epilogue.h`:

```c
/* Check that all frame size usages are consistent */
int prologue_epilogue_consistency_check(ZeroCCCompiler *c) {
    Instr *prologue = ir_get(0);
    size_t prologue_frame_size = (size_t)prologue->imm64;
    
    /* Check machine code emitter sees same frame size */
    /* This is harder to validate without running the lowering,
       so we add assertions in the lowering code instead */
    
    return 1;
}
```

2. Add assertions in `ast_lowering_mc.c`:

```c
case OP_PROLOGUE:
    {
        size_t frame_size = (size_t)instr->imm64;
        
        /* Validate frame size before emitting */
        assert(frame_size % 16 == 0, 
               "Frame size not 16-byte aligned: %zu", frame_size);
        assert(frame_size > 0 && frame_size < (1u << 24),
               "Frame size out of valid range: %zu", frame_size);
        
        if (BE(c)->emit_prologue) {
            BE(c)->emit_prologue(c, frame_size, 0);
        }
    }
    break;
```

3. Add same assertions in `asm_emitter.c`:

```c
case OP_PROLOGUE:
    {
        size_t frame_size = (size_t)instr->imm64;
        
        /* Validate frame size before emitting */
        assert(frame_size % 16 == 0,
               "Frame size not 16-byte aligned: %zu", frame_size);
        assert(frame_size > 0 && frame_size < (1u << 24),
               "Frame size out of valid range: %zu", frame_size);
        
        fprintf(out, "\tpush %%rbp\n");
        fprintf(out, "\tmov %%rsp, %%rbp\n");
        fprintf(out, "\tsub $%zu, %%rsp\n", frame_size);
    }
    break;
```

**Risk**: Low - just adds assertions, doesn't change behavior

**Benefit**: Catches misconfigurations immediately with stack trace

---

### Phase 4: ISA-Agnostic Prologue (3-5 days)

**Goal**: Move hardcoded ISA details to backend abstraction

**Changes**:

1. Add prologue template to backend:

```c
typedef struct {
    /* Prologue components */
    const char *push_reg;         /* e.g., "push %%rbp" */
    const char *mov_reg_reg;      /* e.g., "mov %%rsp, %%rbp" */
    const char *sub_rsp_fmt;      /* e.g., "sub $%%zu, %%rsp" */
    
    /* Epilogue components */
    const char *leave;            /* e.g., "leave" */
    const char *ret;              /* e.g., "ret" */
} ISABasicOps;
```

2. Define for each ISA:

```c
/* x86_64 */
static const ISABasicOps x86_64_basic_ops = {
    .push_reg = "push %%rbp",
    .mov_reg_reg = "mov %%rsp, %%rbp",
    .sub_rsp_fmt = "sub $%zu, %%rsp",
    .leave = "leave",
    .ret = "ret",
};

/* aarch64 */
static const ISABasicOps aarch64_basic_ops = {
    .push_reg = "stp %%fp, %%lr, [%%sp, #-16]!",
    .mov_reg_reg = "mov %%fp, %%sp",
    .sub_rsp_fmt = "sub %%sp, %%sp, #%zu",
    .leave = "ldp %%fp, %%lr, [%%sp], #16",
    .ret = "ret",
};
```

3. Use in assembly emitter:

```c
case OP_PROLOGUE:
    {
        size_t frame_size = (size_t)instr->imm64;
        const ISABasicOps *ops = BE(c)->basic_ops;
        
        fprintf(out, "\t%s\n", ops->push_reg);
        fprintf(out, "\t%s\n", ops->mov_reg_reg);
        fprintf(out, "\t");
        fprintf(out, ops->sub_rsp_fmt, frame_size);
        fprintf(out, "\n");
    }
    break;
```

**Risk**: Medium-high - changes multiple backends

**Benefit**: Prologue/epilogue automatically correct for all ISAs

---

## Implementation Priority

### Critical (Do First)
1. **Phase 1**: Add validation - catches bugs immediately, low risk
2. **Phase 3**: Add assertions - prevents silent failures

### Important (Do Soon)
3. **Phase 2**: Centralize frame size - single source of truth
4. **Phase 4**: ISA-agnostic prologue - fixes for all backends

### Nice to Have (Later)
5. Document the frame layout invariants
6. Add comprehensive frame layout tests
7. Add CI check for frame alignment

---

## Testing Plan

### Unit Tests

```bash
# Test frame layout calculation
test_frame_layout_no_spill
test_frame_layout_with_spill
test_frame_layout_alignment_invariant
test_frame_layout_consistency

# Test prologue validation
test_prologue_validate_correct
test_prologue_validate_misaligned
test_prologue_validate_missing
```

### Integration Tests

```bash
# Test prologue/epilogue in machine code
test_prologue_machine_code_x86_64
test_prologue_machine_code_aarch64
test_prologue_machine_code_riscv64

# Test prologue/epilogue in assembly
test_prologue_assembly_x86_64
test_prologue_assembly_aarch64
test_prologue_assembly_riscv64

# Test with various frame sizes
test_frame_sizes_0_to_256_bytes
test_frame_sizes_with_various_alignments
```

### Regression Tests

```bash
# Ensure existing tests still pass
make test  # All 118/118 tests should pass
make aot   # AOT compilation tests
make abi   # ABI compliance tests
```

---

## Estimated Timeline

| Phase | Effort | Time | Risk |
|-------|--------|------|------|
| Phase 1 (Validation) | 200 lines | 2-4 hours | Very Low |
| Phase 2 (Centralize) | 300 lines | 2-3 days | Low |
| Phase 3 (Consistency) | 100 lines | 1 day | Very Low |
| Phase 4 (ISA-Agnostic) | 500 lines | 3-5 days | Medium |
| **Total** | **~1100 lines** | **1-2 weeks** | **Low-Medium** |

---

## Success Criteria

- [ ] All 118/118 existing tests pass
- [ ] Prologue/epilogue validation catches misconfigurations
- [ ] Frame size calculated once, used consistently
- [ ] Assembly emitter and machine code emitter produce equivalent prologues
- [ ] Adding new ISA only requires defining prologue template, not duplicating code
- [ ] Frame size invariant documented and tested

---

## Related Documentation

- [Prologue/Epilogue Analysis](./PROLOGUE_EPILOGUE_ANALYSIS.md)
- [System V AMD64 ABI](https://refspecs.linuxbase.org/elf/x86-64-abi-1.0.pdf)
- [Frame Layout in Modern Compilers](https://en.wikipedia.org/wiki/Stack_frame)
