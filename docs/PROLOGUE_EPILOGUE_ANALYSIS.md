# Prologue/Epilogue Design Fragility Analysis

## Executive Summary

The prologue and epilogue are fragile because they involve **multiple disconnected systems** that must stay synchronized:

1. **Frame size calculation** (ast_lowering.c:127-130)
2. **IR instruction emission** (ir.h:427, 432)
3. **IR-to-machine-code lowering** (ast_lowering_mc.c)
4. **IR-to-assembly lowering** (asm_emitter.c)
5. **ISA-specific backend emission** (x86_64.c, aarch64.c, riscv.c)
6. **Register allocation** (regalloc.c, live_range.h)
7. **Stack layout assumptions** (throughout the codebase)

If any of these get out of sync, the result is **stack corruption, segfaults, or incorrect results**.

---

## The Five Fragile Synchronization Points

### 1. Frame Size Calculation (The Fragile Center)

**Location**: `src/zero_cc/parser/ast_lowering.c:115-135`

```c
/* Allocate frame spill slots for any temps that were spilled */
c->current_stack_frame = (c->current_stack_frame + 7) & ~7;

if (c->current_stack_frame < CALLEE_SAVE_FRAME_RESERVE) {
    c->current_stack_frame = CALLEE_SAVE_FRAME_RESERVE;
}

int32_t spill_base_offset = c->current_stack_frame;

if (c->regalloc.next_spill_slot > 0) {
    c->current_stack_frame += c->regalloc.next_spill_slot * 8;
}

/* Calculate total 16-byte aligned stack frame size */
size_t total_frame_size = (size_t)((c->current_stack_frame + 15) & ~15);

/* Patch OP_PROLOGUE with the calculated frame size */
Instr *prologue_instr = ir_get(0);
if (prologue_instr && prologue_instr->op == OP_PROLOGUE) {
    prologue_instr->imm64 = (uint64_t)total_frame_size;
}
```

**Why It's Fragile**:
- **No validation**: Just assumes `ir_get(0)` is actually the prologue instruction
- **No error handling**: If it's not, silently proceeds with wrong frame size
- **Magic constants**: `CALLEE_SAVE_FRAME_RESERVE` is defined elsewhere, easy to forget to update
- **Multiple calculations**: Frame size touches at least 4 different variables (current_stack_frame, CALLEE_SAVE_FRAME_RESERVE, next_spill_slot, total_frame_size)
- **Alignment logic scattered**: 8-byte alignment (line 117), 16-byte alignment (line 135)

### 2. IR Prologue/Epilogue Emission (The Silent Assumption)

**Location**: `src/zero_cc/parser/ir.h:427-432`

```c
void ir_emit_prologue(size_t frame_size) {
    ir_emit((Instr){
        .op = OP_PROLOGUE,
        .temp_dest = 0xFF, .temp_src1 = 0xFF, .temp_src2 = 0xFF,
        .imm = 0,
        .imm64 = frame_size,  /* <-- WILL BE OVERWRITTEN LATER */
        .label_idx = 0
    });
}

void ir_emit_epilogue(void) {
    ir_emit((Instr){
        .op = OP_EPILOGUE,
        .temp_dest = 0xFF, .temp_src1 = 0xFF, .temp_src2 = 0xFF,
        .imm = 0,
        .label_idx = 0
    });
}
```

**Why It's Fragile**:
- **Two-phase emission**: Prologue emitted early (before register allocation), but frame size patched later
- **No synchronization**: Frame size passed to `ir_emit_prologue()` is IGNORED
- **Hidden overwrite**: Frame size patched at line 135, but callers don't know about it
- **Brittle assumption**: Code assumes prologue is at index 0 (first instruction) - what if it's not?
- **No bounds check**: If IR buffer is empty, `ir_get(0)` returns garbage

### 3. Machine Code Lowering (The Dispatcher)

**Location**: `src/zero_cc/parser/ast_lowering_mc.c`

```c
case OP_PROLOGUE:
    /* Emit prologue with stack frame size (callee-save handled by separate IR instructions) */
    if (BE(c)->emit_prologue) {
        BE(c)->emit_prologue(c, (size_t)instr->imm64, 0);
    }
    break;
```

**Why It's Fragile**:
- **Null check**: Crashes if backend doesn't implement `emit_prologue`
- **Hardcoded callee_save_mask = 0**: Callee-save handling is supposed to be "separate IR instructions", but what if it's not emitted correctly?
- **No validation**: Doesn't check if `instr->imm64` is reasonable (e.g., not > 1MB)
- **Backend abstraction leaks**: Different backends might handle frame size differently

### 4. Assembly Emitter (The Text Output)

**Location**: `src/zero_cc/parser/asm_emitter.c`

```c
case OP_PROLOGUE:
    fprintf(out, "\t# PROLOGUE\n");
    fprintf(out, "\tpush %%rbp\n");
    fprintf(out, "\tmov %%rsp, %%rbp\n");
    fprintf(out, "\tsub $%zu, %%rsp\n", (size_t)instr->imm64);
    return;
```

**Why It's Fragile**:
- **No validation of imm64**: Prints whatever value is there, even if invalid
- **Hardcoded x86-64 registers**: `%rbp`, `%rsp` are hardcoded - assumes x86-64
- **No alignment check**: Doesn't verify that `instr->imm64` is 16-byte aligned
- **Diverges from machine code path**: Slightly different emission than x86_64.c

### 5. Backend-Specific Emission (The ISA-Specific Details)

**Location**: `src/zero_cc/isa/x86_64.c`

```c
static void x86_emit_prologue(ZeroCCCompiler *c, size_t stack_size, uint32_t callee_save_mask) {
    /* push rbp */
    me_emit_u8(c, 0x55);
    /* mov rbp, rsp */
    me_emit_u8(c, 0x48);
    me_emit_u8(c, 0x89);
    me_emit_u8(c, 0xe5);
    /* sub rsp, stack_size (align to 16 bytes) */
    if (stack_size > 0) {
        me_emit_u8(c, 0x48);
        me_emit_u8(c, 0x81);
        me_emit_u8(c, 0xec);
        me_emit_u32(c, (uint32_t)stack_size);
    }
    
    /* Emit callee-saved register stores */
    /* ... complex logic ... */
}
```

**Why It's Fragile**:
- **Hardcoded machine code bytes**: Changes to prologue sequence require careful opcode updates
- **No comments**: What do `0x55`, `0x48`, `0x89`, `0xe5` mean? (push rbp, mov rbp, rsp)
- **Silent truncation**: `(uint32_t)stack_size` truncates 64-bit values silently
- **Callee-save handling**: Complex logic mixed with simple prologue emission
- **No validation**: Doesn't check if `stack_size` is 16-byte aligned

---

## Why This Matters: Real Bugs From Fragility

### Bug #1: Wrong Frame Size Calculation

**Symptom**: JIT test hangs or segfaults, assembly output is misaligned

**Root Cause**: Changed frame size calculation from 16-byte to "8 mod 16", breaking ABI contract

**Why It Happened**: Frame size calculation is disconnected from ABI requirements - it's just math in one place

**How We Found It**: Testing revealed stack misalignment when calling printf

### Bug #2: Prologue Not at Index 0

**Potential Issue**: If IR is structured differently, `ir_get(0)` might not be OP_PROLOGUE

**Why It Hasn't Happened**: Current code always emits prologue first, but there's no guarantee

**Risk**: Changing IR emission order could cause frame size patch to go to wrong instruction

### Bug #3: Callee-Save Handling Mismatch

**Issue**: Machine code path expects callee-save handling via "separate IR instructions"

**Why It's Fragile**: If callee-save IR isn't emitted, frame size is wrong

**Current Status**: Works by accident - callee-save IR is always emitted

### Bug #4: Assembly vs Machine Code Divergence

**Issue**: Assembly emitter outputs different code than machine code emitter

**Example**: Assembly uses `sub $%zu`, machine code uses `me_emit_u32(c, (uint32_t)stack_size)`

**Why It Matters**: If they diverge, assembly output won't match machine code behavior

---

## Root Causes of Fragility

### 1. Distributed Responsibility

**Problem**: Prologue/epilogue logic is spread across 7+ files with no single authority

```
Frame size calc     → ast_lowering.c
IR emission         → ir.h
IR patching         → ast_lowering.c (again!)
Machine emission    → ast_lowering_mc.c
Backend emit        → x86_64.c, aarch64.c, riscv.c
Assembly emit       → asm_emitter.c
Validation          → Nowhere!
```

**Solution**: Centralize prologue/epilogue logic into a single module

### 2. No Validation

**Problem**: No checks that frame size is:
- 16-byte aligned
- Not > stack limit
- Not negative
- Consistent across IR and machine code

**Solution**: Add assertions and validation functions

### 3. Magic Constants Scattered

**Problem**: `0xFF` (frame pointer marker), `CALLEE_SAVE_FRAME_RESERVE`, hardcoded register numbers

**Solution**: Define all constants in one header with documentation

### 4. Two-Phase Frame Size

**Problem**: Frame size passed to `ir_emit_prologue()`, then OVERWRITTEN later

**Solution**: Either:
- Calculate frame size BEFORE emitting prologue, or
- Make frame size a property of the compilation context

### 5. Hardcoded ISA Assumptions

**Problem**: Assembly emitter assumes x86-64 registers (%rbp, %rsp)

**Solution**: Use backend abstraction to get prologue template

---

## Recommended Fixes

### Phase 1: Validation (Low Risk, High Value)

Add validation function before lowering:

```c
static void validate_prologue(ZeroCCCompiler *c) {
    Instr *prologue = ir_get(0);
    
    // Check prologue exists
    if (!prologue || prologue->op != OP_PROLOGUE) {
        fprintf(stderr, "ERROR: First instruction is not OP_PROLOGUE\n");
        exit(1);
    }
    
    // Check frame size is valid
    size_t frame_size = (size_t)prologue->imm64;
    if (frame_size % 16 != 0) {
        fprintf(stderr, "ERROR: Frame size %zu not 16-byte aligned\n", frame_size);
        exit(1);
    }
    
    if (frame_size > (1u << 24)) {  // 16MB limit
        fprintf(stderr, "ERROR: Frame size %zu exceeds 16MB limit\n", frame_size);
        exit(1);
    }
}
```

### Phase 2: Centralize Frame Size Calculation (Medium Risk, High Value)

Create `prologue_epilogue.c`:

```c
typedef struct {
    size_t spill_bytes;
    size_t callee_save_bytes;
    size_t param_save_bytes;
    size_t total_frame_size;
    int32_t spill_base_offset;
} FrameLayout;

FrameLayout calculate_frame_layout(
    ZeroCCCompiler *c,
    uint32_t callee_save_mask,
    int next_spill_slot
) {
    // Single source of truth for frame size calculation
    // ...
    return layout;
}
```

### Phase 3: Abstract Backend Prologue (Medium Risk, Medium Value)

Instead of hardcoded instructions, define prologue template:

```c
typedef struct {
    const char *prologue_template;  // "%s, %s, %s, $%%zu, %s"
    const char *push_reg;           // "push %%rbp"
    const char *mov_reg_reg;        // "mov %%rsp, %%rbp"
    const char *sub_rsp;            // "sub $%%zu, %%rsp"
    // ... other prologue components
} BackendPrologueTemplate;
```

### Phase 4: Consistent IR Emission (Medium Risk, Low Value)

Make frame size a context property:

```c
struct ZeroCCCompiler {
    // ...
    size_t frame_layout;  // Set once, read many times
    // ...
};
```

---

## Testing Strategy

### Unit Tests for Frame Layout Calculation

```c
void test_frame_layout_no_spill(void) {
    FrameLayout layout = calculate_frame_layout(c, 0, 0);
    assert(layout.total_frame_size % 16 == 0);
    assert(layout.total_frame_size >= CALLEE_SAVE_FRAME_RESERVE);
}

void test_frame_layout_with_spill(void) {
    FrameLayout layout = calculate_frame_layout(c, 0, 5);  // 5 spill slots = 40 bytes
    assert(layout.total_frame_size % 16 == 0);
    assert(layout.spill_base_offset >= CALLEE_SAVE_FRAME_RESERVE);
}

void test_frame_layout_alignment(void) {
    for (int spills = 0; spills < 20; spills++) {
        FrameLayout layout = calculate_frame_layout(c, 0, spills);
        assert(layout.total_frame_size % 16 == 0, "Spills: %d", spills);
    }
}
```

### Integration Tests for Prologue/Epilogue

```c
void test_prologue_epilogue_x86_64(void) {
    ZeroCCCompiler *c = create_test_compiler(ZEROCC_ISA_X86_64);
    compile_simple_function(c, "int f() { return 42; }");
    
    // Check prologue sequence
    uint8_t *code = c->text_section.buf;
    assert(code[0] == 0x55);        // push rbp
    assert(code[1] == 0x48);        // mov rbp, rsp
    assert(code[2] == 0x89);
    assert(code[3] == 0xe5);
}
```

---

## Summary: Why Prologue/Epilogue Are Fragile

| Aspect | Problem | Impact | Fix |
|--------|---------|--------|-----|
| **Distributed responsibility** | Logic in 7+ files | Easy to miss dependency | Centralize to prologue_epilogue.c |
| **No validation** | Frame size never checked | Stack corruption | Add assertions |
| **Magic constants** | Hardcoded values everywhere | Easy to update wrong place | Define in one header |
| **Two-phase frame size** | Calculate once, overwrite later | Confusing data flow | Make frame_size immutable |
| **Hardcoded ISA details** | x86-64 assumptions in generic code | Breaks on other ISAs | Use backend abstraction |
| **Silent failures** | No error reporting | Hard to debug | Add validation before lowering |

The key insight: **Prologue/epilogue is fundamentally a multi-stage process (parse → IR → lower → emit), and each stage has different constraints. The current design doesn't enforce consistency across stages.**
