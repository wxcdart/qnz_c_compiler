# LICM Trampoline Design Review

## Executive Summary

**Current Status**: Detection working (324 invariants found), code motion disabled  
**Root Cause**: Flat array IR with index-based addressing breaks on any insertion  
**Proposed Solution**: Trampoline approach with single splice + index update  
**Risk Level**: Medium - requires careful implementation and testing  

---

## The Core Problem

### Flaw A: Pass Manager Fixed-Point Metric ❌

**Issue**: Instruction count changes trigger re-runs → infinite loop

```c
do {
    changed = false;
    for each pass {
        run_pass();
        if (IR changed) changed = true;  // ← Any insertion triggers re-run
    }
} while (changed);  // ← LICM runs forever!
```

**Current Status**: Not addressed in current design

**Fix Needed**: Track specific changes, not just instruction count

---

### Flaw B: Array Splicing Corrupts ALL Cached Indices ❌

**Issue**: ONE splice breaks IRCallArgs, LiveRange, Jump targets

```
Before splice at index 2:
[0] PROLOGUE
[1] MOV_IMM r16, 10
[2] JMP_LABEL 5      ← Jump target = 5
[3] ADD r17, r18, r19
[4] ADD r20, r21, r22
[5] RET              ← Target of jump

After splice at index 2:
[0] PROLOGUE
[1] MOV_IMM r16, 10
[2] NEW_INSTR        ← Inserted here
[3] JMP_LABEL 5      ← Now points to ADD (was RET!)
[4] ADD r17, r18, r19
[5] ADD r20, r21, r22
[6] RET              ← Was at 5, now at 6!
```

**Current Design**: Acknowledges this but proposes "single splice" workaround

**Problem**: Even ONE splice breaks:
- Jump targets (OP_JMP, OP_JE, etc.)
- Live range boundaries
- Call argument tracking (IRCallArgs.instr_index)
- Loop body indices (loop->body_indices[])

---

## Design Options Analyzed

### Option 1: Preheader Insertion (Still Splices) ❌

**From LICM_TRAMPOLINE_DESIGN.md**:
```
Before: [preheader] [LABEL header] [body] [backedge]
After:  [preheader] [JMP hoisted] [LABEL header] [body NOPs] ... [hoisted] [JMP header]
```

**Problem**: Still splices before header! Header index shifts.

**Verdict**: ❌ Doesn't solve the problem

---

### Option 2: Backedge Modification ❌

**From LICM_TRAMPOLINE_DESIGN.md**:
```
Before: [preheader] [LABEL header] [body] [JMP header]
After:  [preheader] [LABEL header] [body NOPs] ... [hoisted] [JMP header] [JMP header]
```

**Problem**: Backedge now goes to hoisted code, not header! Changes loop semantics.

**Verdict**: ❌ Changes control flow

---

### Option 3: First-Entry Trapping ❌

**From LICM_TRAMPOLINE_DESIGN.md**:
```
Before: [LABEL header] [body] [JMP header]
After:  [LABEL header] [body with first-instr=JMP hoisted] ... [hoisted] [JMP header]
```

**Problem**: "First instruction would always jump!" - as noted in design doc

**Verdict**: ❌ Doesn't work

---

### Option 4: Flag-Based Trampoline ⚠️

**From LICM_TRAMPOLINE_DESIGN.md**:
```c
MOV r_temp, 1          // Before loop: set flag
[LABEL header]
CMP r_temp, 0
JE skip_hoisted
[hoisted code]
MOV r_temp, 0
skip_hoisted:
[loop body]
```

**Pros**: No splicing, preserves indices  
**Cons**: 
- Requires extra temp register
- Adds conditional logic to every iteration
- Slows down loop (branch on every iteration)
- Defeats purpose of optimization!

**Verdict**: ⚠️ Works but defeats optimization purpose

---

### Option 5: Label-Based Entry Point ✅

**From LICM_TRAMPOLINE_DESIGN.md**:
```
Before:
[LABEL preheader]
... preheader code ...
[LABEL header]
... body ...
[JMP header]

After:
[LABEL preheader]
... preheader code ...
[JMP header]           <- Insert jump to skip hoisted code on first entry
[LABEL hoisted]        <- Hoisted code (appended to END)
... hoisted instructions ...
[JMP header]           <- Jump back to loop header
[LABEL header]         <- Loop header (unchanged position!)
... body with NOPs ...
[JMP header]
```

**Claim**: "Insert the jump at the END of preheader, not at the header!"

**Problem**: This STILL requires inserting an instruction, which shifts everything after it!

```
Before:
[10] preheader_instr
[11] LABEL header      ← header_idx = 11

After inserting at 11:
[10] preheader_instr
[11] JMP hoisted       ← Inserted here
[12] LABEL header      ← Was at 11, now at 12!
```

**Verdict**: ❌ Still breaks indices (design doc is incorrect)

---

### Option 6: Single Splice + Index Update ⚠️

**From LICM_TRAMPOLINE_DESIGN.md** (Recommended):
```c
// Step 1: Insert jump at preheader
ir_insert_at(loop->header_idx, jump_to_hoisted);

// Step 2: Update loop header index (it shifted by +1)
loop->header_idx++;

// Step 3: Append hoisted to END
ir_emit(hoisted_instr1);
...

// Step 4: Replace invariants with NOP
// (indices are still valid because we only inserted ONE instruction)
```

**Claim**: "ONE splice, update indices, rest is appends"

**Problem**: Even ONE splice breaks:
1. **All jump targets** that point to indices >= splice point
2. **IRCallArgs** with instr_index >= splice point
3. **LiveRange** boundaries
4. **All loop body_indices** (need to shift by +1)

**Partial Fix**: Yes, we can update `loop->header_idx++`, but what about:
- Jump targets in other loops?
- IRCallArgs?
- Live ranges?
- Other cached indices?

**Verdict**: ⚠️ Partially works but requires updating ALL cached indices

---

## The Fundamental Issue

### The Design Docs Are Inconsistent

**LICM_TRAMPOLINE_DESIGN.md says**:
> "The ONE splice at preheader is acceptable because: It's only ONE instruction, We can update the loop header index after the splice"

**But LICM_IMPLEMENTATION_NOTES.md says**:
> "When we insert an instruction at position N, all instructions at position N and beyond shift by +1. This invalidates all cached instruction indices"

**Contradiction**: If ONE splice invalidates ALL indices (as correctly stated in implementation notes), then the "single splice" approach doesn't work!

---

## Correct Analysis

### What Gets Broken by ONE Splice

When we insert at index `N`:

| Data Structure | Field | Broken? | Fix Required |
|----------------|-------|---------|--------------|
| **Jump Targets** | `label_idx` | ✅ Yes | Update all jumps pointing to >= N |
| **Loop Info** | `header_idx` | ✅ Yes | Increment by 1 |
| **Loop Info** | `body_indices[]` | ✅ Yes | Increment all by 1 |
| **IRCallArgs** | `instr_index` | ✅ Yes | Update all >= N |
| **LiveRange** | `start`, `end` | ✅ Yes | Update all >= N |
| **Other Loops** | Any index | ✅ Yes | Update all >= N |

### The "Single Splice" Fallacy

**Claim**: "Only ONE splice, so only ONE index to update"

**Reality**: ONE splice breaks ALL indices pointing to that region, which could be:
- Dozens of jump targets
- Hundreds of call argument entries
- Thousands of live range boundaries

**Example**:
```
[0-100]   Function prologue and setup
[101]     CALL some_function       ← IRCallArgs.instr_index = 101
[102-200] More code
[201]     CALL another_function    ← IRCallArgs.instr_index = 201
[202-300] Loop 1
[301-400] Loop 2
[401]     CALL third_function      ← IRCallArgs.instr_index = 401

Insert at index 150:
[150]     NEW_INSTR                ← Inserted

Now ALL jump targets, call args, live ranges at 150+ are broken!
- IRCallArgs[101] still points to 101 (now correct instruction)
- IRCallArgs[201] still points to 201 (now wrong instruction)
- IRCallArgs[401] still points to 401 (now wrong instruction)
- All jump targets to 150+ are wrong
- All live ranges starting at 150+ are wrong
```

---

## Recommended Solution

### Option A: Two-Pass Rebuild (Recommended) ✅

**Approach**:
1. **Pass 1**: Analyze, collect all invariants, mark for hoisting
2. **Pass 2**: Build NEW IR with hoisted code in correct positions
3. Replace old IR with new IR

**Implementation**:
```c
void opt_licm(ZeroCCCompiler *c) {
    // Pass 1: Analysis
    LoopAnalysisState state;
    analyze_loops(&state);
    mark_invariants(&state);
    
    // Pass 2: Build new IR
    IRBuilder builder;
    ir_builder_init(&builder);
    
    for (size_t i = 0; i < ir_get_count(); i++) {
        Instr *instr = ir_get(i);
        
        // If this is a loop header, emit hoisted code first
        if (is_loop_header(i)) {
            emit_hoisted_code(&builder, get_loop(i));
        }
        
        // Emit current instruction (or NOP if invariant)
        if (is_invariant(i)) {
            ir_builder_emit_nop(&builder);
        } else {
            ir_builder_emit(&builder, *instr);
        }
    }
    
    // Replace old IR with new
    ir_replace_with_builder(&builder);
    ir_builder_free(&builder);
}
```

**Pros**:
- ✅ No index invalidation (new IR has fresh indices)
- ✅ Clean separation of analysis and transformation
- ✅ Well-understood compiler pattern
- ✅ Works with flat array IR

**Cons**:
- ⚠️ Requires IRBuilder API (1-2 weeks to implement)
- ⚠️ O(n) memory for new IR buffer
- ⚠️ Need to update all passes to use builder API

**Timeline**: 1-2 weeks

---

### Option B: Label-Based References (Alternative) ✅

**Approach**: Change from index-based to label-based references

**Current**:
```c
OP_JMP with label_idx = 42  // Absolute index
loop->header_idx = 42       // Absolute index
```

**After**:
```c
OP_JMP with label_idx = "loop_header"  // Symbolic label
loop->header_label = "loop_header"     // Symbolic label
```

**Implementation**:
1. Add label names to IR (not just indices)
2. Resolve labels to indices after all modifications
3. Use resolved indices only during code generation

**Pros**:
- ✅ No index invalidation (labels are stable)
- ✅ More natural for programmers
- ✅ Easier to debug

**Cons**:
- ⚠️ Major IR redesign (3-4 weeks)
- ⚠️ Breaks existing code
- ⚠️ More complex label resolution

**Timeline**: 3-4 weeks

---

### Option C: Accept Detection-Only Mode ⚠️

**Approach**: Keep LICM as detection-only, don't move code

**Pros**:
- ✅ No implementation risk
- ✅ Still useful for profiling
- ✅ DCE can eliminate some invariants

**Cons**:
- ❌ No performance improvement
- ❌ Wasted detection effort
- ❌ 324 invariants found but 0 moved

**Verdict**: ⚠️ Acceptable fallback if Options A/B are too expensive

---

## Recommendation

### Short-Term (This Sprint)

**Implement Option A: Two-Pass Rebuild**

**Why**:
1. Clean solution that actually works
2. 1-2 weeks effort (reasonable)
3. No major IR redesign
4. Enables real LICM optimization

**Tasks**:
1. Create IRBuilder API (~500 lines)
2. Modify LICM to use builder (~300 lines)
3. Update all passes to work with builder (~200 lines)
4. Test thoroughly (1 week)

### Long-Term (Future)

**Consider Option B: Label-Based References**

**Why**:
1. More elegant solution
2. Better for debugging
3. Enables other optimizations

**When**: When we have 3-4 weeks for IR redesign

---

## Critical Bugs in Current Design

### Bug 1: Contradictory Claims

**LICM_TRAMPOLINE_DESIGN.md**: "ONE splice is acceptable"  
**LICM_IMPLEMENTATION_NOTES.md**: "ONE splice breaks ALL indices"

**Fix**: Update design doc to acknowledge the real problem

### Bug 2: Missing Index Updates

**Claim**: "Update loop->header_idx++"  
**Reality**: Need to update ALL cached indices (jumps, call args, live ranges)

**Fix**: Either use Two-Pass Rebuild or implement comprehensive index update logic

### Bug 3: Pass Manager Infinite Loop

**Issue**: Instruction count changes trigger re-runs  
**Fix**: Track specific changes, not just instruction count

---

## Conclusion

**The current trampoline design is flawed**. The "single splice" approach doesn't work because:

1. ONE splice breaks ALL cached indices (not just header_idx)
2. The design docs contradict themselves on this point
3. Updating all indices is more work than Two-Pass Rebuild

**Recommended**: Implement Two-Pass Rebuild (Option A)

**Timeline**: 1-2 weeks  
**Risk**: Medium (well-understood pattern)  
**Benefit**: Real LICM optimization with 20-30% ROI for loop-heavy code

---

## Next Steps

1. **Update Design Docs**: Fix contradictions, acknowledge real problem
2. **Implement IRBuilder**: Create builder API for safe IR construction
3. **Modify LICM**: Use Two-Pass approach
4. **Test Thoroughly**: Verify no index corruption
5. **Profile Results**: Measure actual performance improvement

**Ready to implement when prioritized!** 🎯
