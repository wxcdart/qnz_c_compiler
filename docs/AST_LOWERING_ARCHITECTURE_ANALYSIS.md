# AST Lowering Architecture Analysis

## Current State

### ISA-Specific Code Locations

| File | Issue | Status |
|------|-------|--------|
| `ast_lowering.c` | Callee-saved register handling (x86-64 only) | ✅ Fixed (ISA-conditional) |
| `ast_lowering.c` | Register allocator ISA info | ✅ Fixed (target_isa field) |
| `ast_lowering_expr.c` | Stack slot management | ⚠️ Potential issue |
| `ast_lowering_expr.c` | Value reload logic | ❌ **BUG: Missing reloads** |

### The Bug Pattern

```
IR for main():
  1: MOV_IMM r16, 42        ; acc = 42
  2: MOV_MEM [sp-...], r16  ; store acc to memory
  ...
  9: ADD r21, r17, r20      ; BUG: r17 (flags) used instead of reloading r16
```

**Root Cause**: AST lowering stores values to memory but doesn't reload them before use.

## Architecture Question: Should AST Lowering Be ISA-Agnostic?

### Option 1: Keep AST Lowering ISA-Agnostic (Current Design)

**Pros:**
- Cleaner separation: AST lowering generates IR, backend generates machine code
- Same IR for all ISAs
- Easier to reason about IR semantics
- Optimizations work on ISA-neutral IR

**Cons:**
- AST lowering must manage stack slots and reloads
- Complex logic for temporary management
- Bugs like missing reloads affect all ISAs
- Stack frame layout decisions in AST lowering

**Current Issues:**
- Missing reload logic in `ast_lowering_expr.c`
- Stack slot offset management is complex
- Temporary value lifecycle tracking is error-prone

### Option 2: Move More Logic to Backend

**Pros:**
- ISA-specific decisions (stack layout, calling convention) in backend
- Backend knows register pressure and can optimize reloads
- Cleaner separation of concerns
- Easier to add new ISAs

**Cons:**
- IR becomes less explicit about memory operations
- Backend must understand value lifetimes
- More complex backend implementation
- IR optimizations become harder

**What Would Move:**
- Stack slot allocation
- Value spilling and reloading
- Calling convention details
- Callee-saved register handling

### Option 3: Hybrid Approach (Recommended)

**Keep in AST Lowering:**
- IR generation (MOV, ADD, CALL, etc.)
- Expression evaluation order
- Temporary variable creation
- Semantic analysis results

**Move to Backend:**
- Stack slot allocation
- Value spilling/reloading decisions
- Register pressure management
- ISA-specific calling convention details

**Benefits:**
- IR remains explicit and optimizable
- Backend handles ISA-specific decisions
- Cleaner separation
- Easier to debug

## Recommended Architecture Changes

### Phase 1: Fix Current Bug (Immediate)

**File**: `src/zero_cc/parser/ast_lowering_expr.c`

**Issue**: Missing reload before ADD operation

**Fix**: Add reload logic when value is needed from memory

```c
// Before (buggy):
// Store accumulator to memory
ir_emit_store(acc_temp, BE(c)->fp_reg, -sym->stack_offset);

// Later - use without reload (BUG!)
ir_emit_add(result_temp, flags_temp, return_temp);

// After (fixed):
// Store accumulator to memory
ir_emit_store(acc_temp, BE(c)->fp_reg, -sym->stack_offset);

// Later - reload before use (FIX)
ir_emit_load(acc_temp_reloaded, BE(c)->fp_reg, -sym->stack_offset);
ir_emit_add(result_temp, acc_temp_reloaded, return_temp);
```

### Phase 2: Make Stack Management ISA-Agnostic (Medium-term)

**Current Issue**: Stack slot offsets are managed in AST lowering but layout is ISA-specific.

**Solution**: Add stack management layer

```c
// New file: src/zero_cc/parser/stack_manager.h
typedef struct StackManager {
    int32_t current_offset;
    int32_t callee_save_size;
    int32_t param_area_size;
    // ISA-specific callback
    int32_t (*align_offset)(int32_t offset, ISA isa);
    int32_t (*alloc_slot)(StackManager *sm, int size, ISA isa);
} StackManager;
```

**Benefits:**
- Stack alignment rules in one place
- ISA-specific layout decisions in callbacks
- AST lowering uses simple API

### Phase 3: Move Spilling to Backend (Long-term)

**Current Issue**: AST lowering decides when to store to memory.

**Solution**: Let backend decide spilling based on register pressure

```c
// IR remains the same (explicit MOV_MEM, MOV)
// Backend analyzes register pressure
// Backend inserts reloads where needed

// Backend implementation:
void backend_insert_reloads(Backend *be, IR *ir) {
    // Analyze register pressure at each point
    // Insert reloads when register is needed but value is in memory
}
```

**Benefits:**
- Optimal spilling decisions
- ISA-specific register pressure analysis
- Cleaner IR generation

## Implementation Plan

### Step 1: Fix Missing Reload Bug (1-2 hours)

1. Identify where values are stored to memory
2. Track which values are needed later
3. Insert reload before use
4. Test with accumulator case
5. Verify all 118 JIT tests pass

**Files to Modify:**
- `src/zero_cc/parser/ast_lowering_expr.c` - Add reload logic

### Step 2: Add Stack Manager (2-3 days)

1. Design `StackManager` API
2. Implement for x86-64
3. Implement for AArch64
4. Update AST lowering to use API
5. Test both ISAs

**Files to Create/Modify:**
- `src/zero_cc/parser/stack_manager.h` (new)
- `src/zero_cc/parser/stack_manager.c` (new)
- `src/zero_cc/parser/ast_lowering.c` (modify)
- `src/zero_cc/isa/x86_64.c` (modify)
- `src/zero_cc/isa/aarch64.c` (modify)

### Step 3: Backend Spilling (1-2 weeks)

1. Design backend spilling interface
2. Implement register pressure analysis
3. Add reload insertion in backend
4. Test with complex expressions
5. Verify optimization passes still work

**Files to Modify:**
- `src/zero_cc/isa/isa_backend.h` (add interface)
- `src/zero_cc/isa/x86_64.c` (implement)
- `src/zero_cc/isa/aarch64.c` (implement)
- `src/zero_cc/parser/ir.c` (possibly modify)

## Current Recommendation

**For now, stick with Option 1 (ISA-agnostic AST lowering)** but:

1. ✅ **Fix the immediate bug** in `ast_lowering_expr.c`
2. ✅ **Add proper reload tracking** to prevent similar bugs
3. ⏳ **Consider Phase 2** (stack manager) after IR is stable
4. ⏳ **Defer Phase 3** (backend spilling) until needed

**Why:**
- IR is now ISA-agnostic (flag refactoring complete)
- Register allocator works for multiple ISAs
- Current bug is fixable without architectural changes
- More complex changes should wait until compiler is stable

## Testing Strategy

### Before Changes

```bash
# Test register allocation
make test  # 118/118 pass

# Test AArch64 code generation
./build/bin/zerocc -c test.c -o test.o --target aarch64
qemu-aarch64-static test  # Wrong result (20 instead of 52)
```

### After Bug Fix

```bash
# Test register allocation
make test  # 118/118 pass

# Test AArch64 code generation
./build/bin/zerocc -c test.c -o test.o --target aarch64
qemu-aarch64-static test  # Correct result (52)

# Verify with IR tools
python3 tools/ir_accumulator_bug_detector.py --ir ir_dump.txt
# Should show: No accumulator bugs detected!
```

## Conclusion

**Don't change architecture yet.** The current ISA-agnostic design is correct, but has a bug in the reload logic.

**What to do:**
1. Fix the missing reload bug in `ast_lowering_expr.c`
2. Add better tracking to prevent similar bugs
3. Re-evaluate architecture after IR is stable

**What NOT to do:**
- Don't move spilling to backend yet (too complex)
- Don't change IR design (it's working)
- Don't duplicate logic in backend (violates separation)

**When to reconsider:**
- If similar reload bugs keep happening
- If register pressure analysis becomes critical
- If adding new ISAs becomes too difficult
