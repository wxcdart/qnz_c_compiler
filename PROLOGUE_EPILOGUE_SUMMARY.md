# Why Prologue/Epilogue is Fragile - Executive Summary

## The Core Problem

The prologue and epilogue (function entry/exit code) involve **5 disconnected systems that must stay perfectly synchronized**:

1. **Frame size calculation** (happens in ast_lowering.c, AFTER register allocation)
2. **IR instruction emission** (happens BEFORE frame size is known)
3. **Frame size patching** (overwrites the IR AFTER calculation)
4. **Machine code lowering** (reads the patched IR)
5. **Assembly emission** (separate code path, also reads IR)

If any of these get out of sync, you get **stack corruption, segfaults, or silent wrong answers**.

---

## Five Fragile Synchronization Points

### 1. Frame Size is Calculated Late, Patched Retroactively

**The Code** (`ast_lowering.c:127-135`):
```c
// Frame size calculated HERE, AFTER register allocation
size_t total_frame_size = (size_t)((c->current_stack_frame + 15) & ~15);

// Patched INTO the IR instruction HERE
Instr *prologue_instr = ir_get(0);
if (prologue_instr && prologue_instr->op == OP_PROLOGUE) {
    prologue_instr->imm64 = (uint64_t)total_frame_size;  // Overwrite!
}
```

**The Problem**:
- Prologue is emitted EARLY (before register allocation) with placeholder frame size
- Frame size calculated LATE (after register allocation)
- Then OVERWRITTEN into the IR
- If register allocation changes, frame size changes, but nobody validates it

**Fragility**:
- ❌ No check that `ir_get(0)` is actually `OP_PROLOGUE`
- ❌ No check that new frame size is 16-byte aligned
- ❌ No check that frame size is reasonable (< 16MB)
- ❌ No check that it matches what assembly emitter expects

### 2. Prologue Assumed to Be First Instruction

**The Code** (`ast_lowering.c:135`):
```c
Instr *prologue_instr = ir_get(0);  // Assume first instruction is prologue
```

**The Problem**:
- If IR structure changes, prologue might not be at index 0
- Silent failure - no error message, just wrong frame size

**When It Could Break**:
- Add per-function IR preamble (debug info, metadata, etc.)
- Change order of IR emission
- Add function attributes

### 3. Multiple Frame Size Calculations

**The Code** (`ast_lowering.c:115-135`):
```c
c->current_stack_frame = (c->current_stack_frame + 7) & ~7;      // 8-byte align
if (c->current_stack_frame < CALLEE_SAVE_FRAME_RESERVE) { ... }  // Adjust for callee-save
int32_t spill_base_offset = c->current_stack_frame;
if (c->regalloc.next_spill_slot > 0) {
    c->current_stack_frame += c->regalloc.next_spill_slot * 8;   // Add spill slots
}
size_t total_frame_size = (size_t)((c->current_stack_frame + 15) & ~15);  // 16-byte align
```

**The Problem**:
- 5 different calculations involving 4 different variables
- Magic constants scattered throughout
- No single place that explains the overall strategy
- Easy to miss one when making changes

**Example Fragility**: When I changed the alignment, I broke JIT because I didn't understand that `current_stack_frame` was already 8-byte aligned.

### 4. Assembly Emitter Has Its Own Prologue Code

**Machine Code Path** (`ast_lowering_mc.c`):
```c
case OP_PROLOGUE:
    if (BE(c)->emit_prologue) {
        BE(c)->emit_prologue(c, (size_t)instr->imm64, 0);
    }
```

**Assembly Path** (`asm_emitter.c`):
```c
case OP_PROLOGUE:
    fprintf(out, "\tpush %%rbp\n");
    fprintf(out, "\tmov %%rsp, %%rbp\n");
    fprintf(out, "\tsub $%zu, %%rsp\n", (size_t)instr->imm64);
```

**The Problem**:
- Two separate implementations
- If one changes, other must also change
- x86_64.c has the actual machine code version
- asm_emitter.c has a text version
- Easy to diverge

**Fragility**:
- Changed alignment calculation in one place, broke the other
- No automated check that they produce equivalent results

### 5. Callee-Save Handling is "Separate IR Instructions"

**The Code** (`ast_lowering.c:111-113`):
```c
uint32_t callee_save_mask = c->regalloc.used_callee_saved_gpr_mask;
if (callee_save_mask) {
    ir_emit_callee_save_stores(callee_save_mask);
    ir_emit_callee_save_loads(callee_save_mask);
}
```

**The Problem**:
- Callee-save handling is supposed to be "separate IR instructions"
- But prologue assumes it's handled elsewhere
- If callee-save IR isn't emitted correctly, frame size is wrong
- No validation that they're consistent

---

## Why This Matters: A Real Bug

### What Happened
1. I added conditional frame size calculation for assembly emitter
2. JIT path used one calculation, assembly path used another
3. Assembly path was 8 mod 16, JIT path was 16 mod 16
4. Assembly output had `sub $72, %rsp` (misaligned!)
5. Calling `printf()` from assembly caused segfault due to stack misalignment

### Why It Happened
- Didn't understand that System V ABI requires 16-byte alignment
- Two separate frame size calculations
- No validation that alignment was correct
- No test for assembly emitter (only JIT/AOT tests)

### The Lesson
The fragility enabled me to make the change without immediately seeing the error. The error only manifested when running the specific test that called a library function requiring correct stack alignment.

---

## The Ideal Design (How It Should Work)

```
┌─────────────────────────────────────┐
│  Frame Layout Calculation (SINGLE)  │
│  - Validates alignment              │
│  - Validates size < 16MB            │
│  - Calculates all components once   │
└─────────────────┬───────────────────┘
                  │
        Result: FrameLayout { size: 64 }
                  │
          ┌───────┴───────┐
          │               │
     Use in IR        Use in Lowering
          │               │
    ┌─────▼──────┐  ┌────▼──────┐
    │ OP_PROLOGUE│  │ Validate  │
    │ imm64=64   │  │ & Emit    │
    └────────────┘  └───────────┘
                       │
            ┌──────────┼──────────┐
            │          │          │
      ┌─────▼──┐ ┌────▼────┐ ┌──▼───┐
      │ x86_64 │ │ aarch64  │ │ asm  │
      │ emit   │ │ emit     │ │emit  │
      └────────┘ └──────────┘ └──────┘
```

**Key Principles**:
1. **Single calculation**: Frame layout calculated once, validated immediately
2. **Early validation**: Check alignment, size, all invariants before use
3. **Immutable after creation**: Frame size never changes or gets patched
4. **Consistent use**: All backends use same calculation
5. **Testable**: Can test frame layout separately from code emission

---

## How to Fix It (4-Phase Plan)

### Phase 1: Add Validation (2-4 hours, Very Low Risk)
- Add `prologue_epilogue_validate()` function
- Check prologue at index 0
- Validate frame size is 16-byte aligned
- Call before lowering to machine code
- **Result**: Catch bugs early with clear error messages

### Phase 2: Centralize Frame Size (2-3 days, Low Risk)
- Create `frame_layout.c` with single `calculate_frame_layout()` function
- Use before emitting prologue IR
- **Result**: Single source of truth for frame size

### Phase 3: Add Assertions (1 day, Very Low Risk)
- Add assertions in machine code emitter
- Add assertions in assembly emitter
- Check frame size is 16-byte aligned
- Check frame size < 16MB
- **Result**: Catch divergence immediately

### Phase 4: ISA-Agnostic Template (3-5 days, Medium Risk)
- Move prologue from hardcoded bytes to template
- Define prologue_template in ISA backend
- Use template in assembly emitter
- **Result**: Changes to prologue affect all ISAs automatically

**Total Effort**: 1-2 weeks for significant improvement

---

## Why Prologue/Epilogue Gets Fragile Over Time

The core issue: **Function prologue/epilogue is fundamentally a multi-stage process**:

1. **Parse**: Know the function exists, don't know stack requirements
2. **Analyze**: Run register allocation, know what needs stack space
3. **Lower to IR**: Emit prologue placeholder with estimated size
4. **Patch IR**: Calculate actual size, overwrite placeholder
5. **Lower to Machine Code**: Read frame size from IR, emit instructions
6. **Optimize**: Potentially change frame size (spill elimination, etc.)

**The Design Problem**: Each stage has different information, different constraints, different assumptions. If they don't synchronize perfectly, you get bugs.

**The Solution**: Make frame layout calculation happen early (after register allocation, before IR emission) so it's immutable and validated once.

---

## Key Takeaways

| Problem | Impact | Root Cause |
|---------|--------|------------|
| Frame size calculated late, patched retroactively | No validation of new size | Doesn't know requirements until after register allocation |
| Prologue assumed at IR index 0 | Silent wrong frame size if structure changes | Hardcoded assumption, no assertion |
| Multiple frame size calculations | Easy to miss one when changing | Responsibility distributed across function |
| Assembly has separate prologue code | Easy to diverge from x86_64 | Code duplication |
| Callee-save handling "separate" | Frame size might be inconsistent with actual needs | Unclear dependency |

---

## Next Steps

1. **Read** `docs/PROLOGUE_EPILOGUE_ANALYSIS.md` for detailed analysis
2. **Review** `docs/PROLOGUE_EPILOGUE_IMPROVEMENT_PLAN.md` for specific code changes
3. **Consider** starting with Phase 1 (validation) - low risk, high value
4. **Test** each phase thoroughly before moving to next
5. **Document** frame layout assumptions clearly

---

## Related Code

- Frame size calculation: `src/zero_cc/parser/ast_lowering.c:115-135`
- Prologue IR: `src/zero_cc/parser/ir.h:427`
- Machine code emission: `src/zero_cc/parser/ast_lowering_mc.c`
- Assembly emission: `src/zero_cc/parser/asm_emitter.c`
- x86_64 backend: `src/zero_cc/isa/x86_64.c`

---

## References

- [System V AMD64 ABI](https://refspecs.linuxbase.org/elf/x86-64-abi-1.0.pdf) - Stack alignment requirements
- [LLVM's PrologEpilogInserter.cpp](https://github.com/llvm/llvm-project/blob/main/llvm/lib/CodeGen/PrologEpilogInserter.cpp) - How a mature compiler handles this
- [GCC's Prolog/Epilog Code](https://gcc.gnu.org/onlinedocs/) - Another reference implementation
