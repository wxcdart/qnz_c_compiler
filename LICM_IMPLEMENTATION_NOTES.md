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

## Recent Implementation Attempt (2024)

### What We Tried

We attempted a **two-pass algorithm** using an IRBuilder:

1. **Pass 1**: Collect all invariants from all loops
2. **Pass 2**: Build new IR with invariants inserted at preheader
3. **Replace**: Atomically swap old IR with new IR

### Why It Failed

Even with the two-pass approach, we encountered an infinite loop:

```
Iteration 1:
  - Detect 5 loops, 324 invariants
  - Build new IR with invariants at position 1
  - Replace IR

Iteration 2:
  - Re-analyze loops (indices shifted!)
  - Detect same 5 loops, same 324 invariants
  - Build new IR again
  - Replace IR
  ...
  Loop forever!
```

### Root Cause

The problem is that **loop analysis is re-run on every optimization iteration**. When we:
1. Insert invariants at position 1
2. Shift all instructions
3. Re-analyze loops

The loop analysis finds the **same loops** with the **same invariants**, even though we already moved them. The optimization loop never terminates.

### Why Detection-Only Works

By keeping detection-only:
- ✅ We can profile optimization opportunities (324 invariants found!)
- ✅ No IR modifications, so no infinite loops
- ✅ DCE still cleans up any dead code

### Future Solutions

To implement full LICM, we need to:

1. **Mark moved invariants** so they're not detected again
2. **Or** change the optimization loop structure to run LICM only once
3. **Or** implement a more sophisticated preheader generation that doesn't conflict with loop analysis

**Timeline**: Requires architectural changes to optimization pipeline
**Complexity**: High (1-2 weeks)
**Priority**: Medium (good optimization, but not blocking)

### Current Status

- **Detection**: ✅ Working (324 invariants found in demo)
- **Code Motion**: ❌ Disabled (causes infinite loops)
- **Instructions Moved**: 0

See `SESSION_SUMMARY.md` for complete analysis.

---

## Latest Attempt: Running LICM Outside Fixed-Point Loop (2024)

### The Analysis

After extensive debugging, the root cause was identified as having **TWO distinct flaws**:

#### Flaw A: Pass Manager Fixed-Point Metric
The pass manager uses `before_count != after_count` to detect changes. When LICM inserts instructions:
- The count changes → triggers another iteration
- Same invariants detected again → inserts again
- Infinite loop!

#### Flaw B: Array Splicing in Flat IR Buffer
When inserting at position 1:
- ALL subsequent instruction indices shift by +1
- Jump targets (OP_JMP, OP_JE, etc.) become corrupted
- Breaks tests WITHOUT loops too!

### What We Tried

**Pattern 1: Run LICM Once Outside Fixed-Point Loop**

```c
// Phase 1: Run local optimizations until fixed-point
while (changed && iteration < max_iterations) {
    opt_constant_fold(c);
    opt_dce(c);
    opt_cse(c);
    // Skip LICM here
}

// Phase 2: Run LICM exactly once
opt_licm(c);

// Phase 3: Final DCE cleanup
opt_dce(c);
```

**Problem**: Even running LICM once causes index corruption because `ir_insert_at(1, ...)` shifts ALL instruction indices.

### Why This Failed

The fundamental problem is **ZeroCC uses a flat array IR**:

```c
typedef struct {
    Instr *buffer;      // Flat array of instructions
    size_t capacity;
    size_t count;
} IRArena;
```

When we insert at position 1:
```
Original: [0]PROLOGUE [1]LABEL [2]ADD [3]JMP
Insert:   [0]PROLOGUE [1]NEW [2]LABEL [3]ADD [4]JMP
Problem:  JMP still points to old index!
```

### The Proper Solution

To implement LICM code motion, we need ONE of these:

#### Option A: Basic Block-Based IR (Architectural Change)
- Replace flat array with linked basic blocks
- Each block has its own instruction list
- Inserting in one block doesn't affect others
- **Complexity**: High (weeks of work)
- **Example**: LLVM, GCC, Clang all use this

#### Option B: Two-Pass with Index Remapping
```c
// Pass 1: Collect all transformations
// Pass 2: Build new IR with correct indices
// Pass 3: Update all jump targets

IRBuilder b;
ir_builder_init(&b);

// Copy instructions, track index mapping
for (int i = 0; i < old_count; i++) {
    Instr *old = ir_get(i);
    size_t new_idx = ir_builder_count(&b);
    index_map[i] = new_idx;
    ir_builder_emit(&b, *old);
}

// Update jump targets using index_map
for (int i = 0; i < new_count; i++) {
    Instr *instr = ir_builder_get(&b, i);
    if (is_jump(instr)) {
        instr->label_idx = index_map[instr->label_idx];
    }
}

ir_builder_finish(&b);
```
- **Complexity**: Medium (1-2 weeks)
- **Risk**: Low (doesn't change IR structure)

#### Option C: Label-Based Preheaders (Simpler)
- Generate a dedicated preheader label before each loop
- Hoist invariants to that preheader (append, don't insert)
- Use label-based jumps instead of index-based
- **Complexity**: Medium (requires label infrastructure)

### Current Decision

**Keep detection-only mode**

**Rationale**:
1. Detection shows real value (324 invariants found!)
2. Code motion requires either:
   - Basic block IR (architectural change, weeks of work)
   - Index remapping (1-2 weeks, non-trivial)
3. Other optimizations have better ROI right now

### Profiling Results (Detection-Only)

```
LICM:
  Loops detected: 5
  Invariants found: 324  ← Real optimization opportunities!
  Instructions moved: 0  ← Detection-only mode

CSE:
  Instructions scanned: 181
  Eliminated: 0
  Elimination rate: 0.00%
```

### Conclusion

LICM code motion is **fundamentally incompatible** with ZeroCC's flat array IR without architectural changes. The detection is working and shows significant optimization opportunities, but exploiting them requires either:

1. **Basic block IR** (weeks of work, architectural change)
2. **Two-pass with index remapping** (1-2 weeks, non-trivial)
3. **Label-based preheaders** (1-2 weeks, requires label infrastructure)

For now, detection-only is the pragmatic choice that:
- ✅ Shows optimization opportunities (324 invariants)
- ✅ Has zero risk of breaking correctness
- ✅ Enables profiling and measurement
- ✅ Can be enhanced later when we have time

**Status**: Detection ✅, Code Motion ❌ (requires architectural changes)
**Timeline**: 1-2 weeks when prioritized

---

## Latest Investigation: Trampoline Approach Analysis (2024)

### User's Excellent Analysis

After the user's deep analysis, we confirmed that **even the trampoline approach (Option C) fails** because:

> When an instruction is physically inserted into a flat array via memmove or splicing, every index > N shifts. Any table that cached an instruction index (call tables, live intervals, label patchers) is immediately desynchronized.

### The Trampoline Design Study

We analyzed 5 different trampoline approaches:

| Option | Approach | Problem |
|--------|----------|---------|
| 1 | Preheader Insertion | Still splices before header |
| 2 | Backedge Modification | Backedge goes to wrong target |
| 3 | First-Entry Trapping | First instruction always jumps |
| 4 | Flag-Based | Requires extra temp + conditional |
| 5 | Label-Based Entry | Still requires ONE splice |

### The Fundamental Problem

**Every approach requires at least ONE splice**, which breaks cached indices:
- `IRCallArgs` with `instr_index`
- `LiveRange` with `def_pos` and `live_end`
- Jump/branch patchers with `label_idx`

### The ONLY Working Solutions

To implement LICM code motion, we need architectural changes:

#### Option A: Basic Block IR (3-4 weeks)
- Replace flat array with linked basic blocks
- No indices to corrupt
- **Example**: LLVM, GCC, Clang

#### Option B: Two-Pass Rebuild with Index Remap (1-2 weeks)
- Build new IR with correct indices
- Update all cached tables (call args, live ranges, labels)
- **Example**: How DCE compacts dead instructions

#### Option C: Label-Based References (1-2 weeks)
- Use `label_idx` instead of instruction indices
- Jumps reference labels, not positions
- **Example**: How assemblers work

### Current Status

**Detection-only mode** is the pragmatic choice:
- ✅ Shows optimization opportunities (324 invariants)
- ✅ Zero risk of breaking correctness
- ✅ Works with current flat array IR
- ✅ Can be enhanced when we do architectural changes

### Profiling Results

```
LICM:
  Loops detected: 5
  Invariants found: 324  ← Real optimization opportunities!
  Instructions moved: 0  ← Detection-only mode
```

### Conclusion

**LICM code motion is fundamentally incompatible with ZeroCC's flat array IR.**

The detection works and shows significant value (324 invariants found). Exploiting them requires either:
1. Basic Block IR (weeks of work)
2. Index remapping (1-2 weeks)
3. Label-based references (1-2 weeks)

For now, detection-only is the right choice that maintains correctness while showing optimization potential.

**Status**: Detection ✅, Code Motion ❌ (requires architectural changes)
**Timeline**: 1-4 weeks depending on approach

---

See `LICM_TRAMPOLINE_DESIGN.md` for detailed trampoline analysis.
