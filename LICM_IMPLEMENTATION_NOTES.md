# LICM Implementation Notes

## Current Status

**Detection**: ✅ Working
- Loops detected: 5
- Invariants found: 324

**Code Motion**: ❌ Not Implemented
- Instructions moved: 0

## Why Code Motion is Not Implemented

### The Problem: Instruction Index Invalidation

When we insert an instruction at position `N`, all instructions at position `N` and beyond shift by +1. This invalidates all cached instruction indices, including:

1. **Loop body indices**: `loop->body_indices[]` becomes stale
2. **Jump targets**: `label_idx` may need updating
3. **Loop boundaries**: `header_idx`, `exit_idx` become incorrect

### Example of the Bug

```c
/* Original IR */
[0] PROLOGUE
[1] LABEL loop_header     <- loop->header_idx = 1
[2] ADD r1, r1, 1         <- loop->body_indices[0] = 2
[3] JMP loop_header       <- loop->body_indices[1] = 3

/* After inserting at position 1 */
[0] PROLOGUE
[1] NEW_INSTR             <- Inserted here
[2] LABEL loop_header     <- Was at 1, now at 2!
[3] ADD r1, r1, 1         <- Was at 2, now at 3!
[4] JMP loop_header       <- Was at 3, now at 4!

/* Problem: loop->header_idx still = 1 (now NEW_INSTR) */
/* Problem: loop->body_indices still = [2, 3] (now wrong instructions) */
```

### Why This Causes Infinite Loops

The optimization passes run in a loop until no changes are detected:

```c
do {
    changed = false;
    for each pass {
        run_pass();
        if (IR changed) changed = true;
    }
} while (changed);
```

If LICM keeps inserting instructions:
1. Pass 1: Insert 10 invariants → IR changes
2. Pass 2: Re-analyze loops → different indices
3. Pass 2: Insert same 10 invariants again → IR changes
4. Loop forever!

## Possible Solutions

### Solution 1: Rebuild IR After All Insertions

**Approach**:
1. Collect all invariants from all loops
2. Insert them all at once
3. Rebuild the entire IR buffer
4. Clear all cached indices

**Pros**:
- Clean, simple logic
- No stale indices

**Cons**:
- O(n) copy of entire IR
- Complex to implement correctly

**Complexity**: Medium

### Solution 2: Use Instruction Pointers Instead of Indices

**Approach**:
1. Store `Instr*` pointers instead of indices
2. When inserting, update all pointers
3. No index invalidation issues

**Pros**:
- No index tracking needed

**Cons**:
- Pointers become invalid after realloc
- Need to track all references

**Complexity**: High

### Solution 3: Two-Pass Algorithm

**Approach**:
1. **Pass 1**: Collect all invariants, mark originals
2. **Pass 2**: Build new IR with invariants at preheader
3. Replace old IR with new IR

**Pros**:
- Clean separation of concerns
- No stale indices

**Cons**:
- Requires IR builder API
- O(n) memory for new IR

**Complexity**: Medium

### Solution 4: Insert at End, Not Beginning

**Approach**:
1. Insert invariants at the END of the IR
2. Use jumps to execute them before the loop
3. No index shift issues for existing code

**Pros**:
- No index invalidation

**Cons**:
- Changes control flow
- Complex jump management

**Complexity**: High

## Recommended Approach: Solution 3 (Two-Pass)

**Why**:
1. Clean separation between analysis and transformation
2. No stale index issues
3. Well-understood pattern in compiler design
4. Can reuse existing IR builder infrastructure

**Implementation Steps**:

1. **Add IR Builder API**:
   ```c
   typedef struct {
       Instr *buffer;
       size_t capacity;
       size_t count;
   } IRBuilder;
   
   void ir_builder_init(IRBuilder *b);
   void ir_builder_emit(IRBuilder *b, Instr instr);
   void ir_builder_insert(IRBuilder *b, size_t pos, Instr instr);
   void ir_builder_finish(IRBuilder *b);  // Replace global IR
   ```

2. **Modify LICM to Use Builder**:
   ```c
   void opt_licm(ZeroCCCompiler *c) {
       IRBuilder builder;
       ir_builder_init(&builder);
       
       // Copy non-invariant instructions
       // Insert invariants at preheader positions
       
       ir_builder_finish(&builder);  // Replace global IR
       ir_builder_free(&builder);
   }
   ```

3. **Update All Passes**:
   - Ensure all passes use `ir_get()` and `ir_emit()`
   - No direct buffer access

**Timeline**: 1-2 weeks

## Current Workaround

For now, LICM **detects invariants but doesn't move them**. This is still useful because:

1. **Profiling**: We can measure how many invariants exist
2. **Future Work**: Detection code is ready for code motion
3. **DCE**: Dead code elimination still works

The 324 invariants detected in our demo show that LICM is finding optimization opportunities, even if we're not exploiting them yet.

## Alternative: Skip LICM, Focus on Other Optimizations

Given the complexity of LICM code motion, we could:

1. **Disable LICM**: Skip the pass entirely
2. **Focus on CSE**: Already working, good ROI
3. **Focus on Cost-Based RegAlloc**: Already enabled, good ROI
4. **Return to LICM Later**: When we have more time

**Decision**: Keep LICM enabled for detection, document that code motion is not yet implemented.

## Conclusion

LICM code motion is a **complex problem** that requires careful handling of instruction indices. The detection is working correctly, but moving code requires either:

1. Rebuilding the entire IR (recommended)
2. Using a different representation (pointers)
3. Complex index tracking

For now, we'll keep the detection working and return to code motion when we have time for a proper two-pass implementation.

**Status**: Detection ✅, Code Motion ❌
**Timeline for Code Motion**: 1-2 weeks (when prioritized)
