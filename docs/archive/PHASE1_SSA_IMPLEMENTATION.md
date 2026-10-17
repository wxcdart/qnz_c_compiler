# Phase 1 SSA Optimization: Implementation Summary

## Strategic Context

Following the recommendation to adopt a **pragmatic 3-phase evolution** toward SSA, we're implementing optimizations that exploit ZeroCC's existing SSA-like temp allocation without full CFG construction.

### Why Not Full Classical SSA (Cytron) Right Now?

Full classical SSA requires:
1. Converting linear IR into explicit Basic Block / CFG structure
2. Dominator tree calculation & Dominance Frontiers algorithm
3. φ-node insertion and variable renaming passes
4. Out-of-SSA destruction with critical-edge splitting

**Impact**: Would stall SIMD emission, AArch64/RISC-V stabilization, and backend completeness for 1-2 months.

### The Recommended 3-Phase Strategy

```
┌──────────────────────────────────────────────────────────┐
│ Phase 1: Capitalize on "Already-SSA" Expressions (Now)   │
│ ➔ Value Numbering (GVN/CSE) on existing linear IR         │
└────────────────────────────┬─────────────────────────────┘
                             │
┌────────────────────────────▼─────────────────────────────┐
│ Phase 2: Lightweight SSA on Basic Blocks (Braun et al.)   │
│ ➔ Direct-from-AST SSA without Dominance Frontiers        │
└────────────────────────────┬─────────────────────────────┘
                             │
┌────────────────────────────▼─────────────────────────────┐
│ Phase 3: Out-of-SSA Coalescing in Linear Scan            │
│ ➔ Map phi-webs directly to VREG intervals                 │
└──────────────────────────────────────────────────────────┘
```

---

## Phase 1: Implementation Status

### What ZeroCC Already Has (SSA-Like)

Every compiler temporary uses `c->next_temp_id++`:

```c
int t1 = c->next_temp_id++;  // LOAD a
int t2 = c->next_temp_id++;  // LOAD b
int t3 = c->next_temp_id++;  // ADD t1, t2
```

**Key Properties**:
- ✅ Each temp is assigned **exactly once** (SSA property)
- ✅ Temps are **immutable** after assignment
- ✅ No φ-nodes needed for expression temps
- ❌ User variables stored to stack ([fp+offset]) are NOT SSA

### What's Implemented

#### 1. mem2reg Pass (New)

**File**: `src/zero_cc/parser/opt_mem2reg.c` (130 lines)

**Purpose**: Eliminate unnecessary stack stores/loads for local variables.

**Algorithm**:
```
1. Track stack slot -> temp mappings
2. OP_MOV_MEM [fp+offset], reg  →  mark slot holds reg
3. OP_LOAD reg, [fp+offset]     →  replace with MOV reg, known_temp
4. Invalidate on calls/control flow
```

**Example**:
```
Before:
  MOV_IMM r1, 42
  MOV_MEM [fp-8], r1    ; store to local x
  LOAD r2, [fp-8]       ; load from local x
  ADD r3, r2, r1

After:
  MOV_IMM r1, 42
  MOV_MEM [fp-8], r1    ; store (kept for address-taking)
  MOV_REG r2, r1        ; load → direct MOV
  ADD r3, r2, r1
```

**Status**: ✅ Implemented, ❌ Disabled (conflicts with VLA)

#### 2. Enhanced CSE/GVN (Deferred)

**Original Plan**:
- Hash (op, src1, src2, imm) → value_number
- Congruence merging for duplicate computations
- Temp remapping to propagate CSE results

**Status**: ❌ Reverted due to bugs

**Future Work**:
- More careful testing
- Better edge case handling
- Incremental enablement

---

## Phase 2: Braun et al. (2013) - Planned

### Why Braun et al. Fits ZeroCC

**Paper**: "Simple and Efficient Construction of Static Single Assignment Form"

**Key Insight**: Construct SSA on-the-fly during AST lowering:
1. Each basic block keeps: `variable_sym → current_temp_id`
2. Write to variable: create new temp, store in block's table
3. Read variable: lookup in current block, then predecessors
   - If all predecessors have same temp → use that temp
   - If different temps → insert φ-node
4. **Zero dominance frontiers to compute**

**Complexity**: ~300 lines of C

### Benefits Over Classical SSA

| Classical (Cytron) | Braun et al. |
|--------------------|--------------|
| Requires CFG construction | Direct from AST |
| Dominator tree needed | No dominators |
| Dominance frontiers | Simple predecessor lookup |
| Complex φ-insertion | On-the-fly φ-insertion |
| ~1000+ lines C | ~300 lines C |

### When to Implement

- After SIMD backends complete
- After AArch64/RISC-V stabilization
- When global SCCP and loop vectorization are needed

---

## Phase 3: Out-of-SSA Coalescing - Planned

### The Problem

SSA φ-nodes don't exist in machine code. Classical approach:
- Insert MOV instructions on control flow edges
- Expensive code bloat
- Complex parallel-copy resolution

### ZeroCC's Solution: Register Allocator Coalescing

**In Linear-Scan RegAlloc** (`regalloc.h`):

```
Instead of runtime MOV instructions:
  Treat all inputs/outputs of φ-node as same congruence class
  Pre-coalesce to share exact same physical register
  If no interference → φ-node costs 0 instructions
```

**Example**:
```
SSA IR:
  t1 = φ(t2, t3)  ; from blocks A and B
  
Register Allocation:
  t1, t2, t3 → same physical register (if no interference)
  
Generated Code:
  No MOV instructions needed!
```

### Benefits

- Zero-cost φ-nodes when possible
- No edge splitting required
- Integrates with existing regalloc.h
- ~50 lines of additional code

---

## Current Status

### Completed (Phase 1)

| Component | Status | Lines |
|-----------|--------|-------|
| mem2reg pass | ✅ Implemented | 130 |
| mem2reg integration | ⚠️ Disabled | - |
| Enhanced CSE/GVN | ❌ Reverted | - |

### Planned (Phase 2)

| Component | Status | Lines |
|-----------|--------|-------|
| Braun SSA construction | 📅 Future | ~300 |
| φ-node insertion | 📅 Future | - |
| SSA-aware optimizations | 📅 Future | - |

### Planned (Phase 3)

| Component | Status | Lines |
|-----------|--------|-------|
| RegAlloc coalescing | 📅 Future | ~50 |
| Critical-edge splitting | 📅 Future | - |

---

## Testing

**All 118/118 tests passing** ✅

---

## Next Steps

1. **Immediate**: Finish SIMD backends and auto-vectorizer
2. **Next Sprint**: Re-implement CSE/GVN with better testing
3. **Future**: Braun SSA when global optimizations are needed

---

## References

- Braun et al. (2013): "Simple and Efficient Construction of Static Single Assignment Form"
- Cytron et al. (1991): "Efficiently Computing Static Single Assignment Form"
- Poletto & Sarkar (1999): "Linear Scan Register Allocation"
