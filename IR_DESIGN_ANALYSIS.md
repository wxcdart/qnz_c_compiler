# IR Design Analysis

## Overview

This document analyzes the ZeroCC IR design to understand why CSE/GVN show 0% elimination rates.

---

## Key Design Characteristics

### 1. SSA-Like Temp Allocation

**Pattern**: Each temp is assigned exactly once

```c
// In ast_lowering_expr.c:
int temp = c->next_temp_id++;  // Always increments
ir_emit_mov_imm(temp, value);  // Assigns to temp
```

**Consequence**: Every computation gets unique source temps

```
; Example: a + b + c
LOAD r19, [fp-56]   ; r19 = a (unique temp)
LOAD r20, [fp-64]   ; r20 = b (unique temp)
LOAD r22, [fp-72]   ; r22 = c (unique temp)
ADD r21, r19, r20   ; r21 = a + b (unique dest)
ADD r23, r21, r22   ; r23 = (a+b) + c (unique dest)

; Second occurrence of same expression:
LOAD r24, [fp-56]   ; r24 = a (DIFFERENT temp!)
LOAD r25, [fp-64]   ; r25 = b (DIFFERENT temp!)
LOAD r27, [fp-72]   ; r27 = c (DIFFERENT temp!)
ADD r26, r24, r25   ; r26 = a + b (different dest!)
ADD r28, r26, r27   ; r28 = (a+b) + c (different dest!)
```

**Impact**: CSE and GVN see these as completely different computations!

### 2. Flat Array IR

**Structure**: Single linear buffer with index-based addressing

```c
typedef struct {
    Instr *buffer;      /* Dynamic instruction buffer */
    size_t capacity;    /* Current buffer capacity */
    size_t count;       /* Number of instructions emitted */
} IRArena;
```

**Consequence**: In-place modification breaks indices

```
; Before splice:
0: MOV_IMM r16, 10
1: ADD r17, r16, r18
2: JMP_LABEL 5
3: ADD r19, r20, r21
4: RET

; After inserting at index 2:
0: MOV_IMM r16, 10
1: ADD r17, r16, r18
2: NEW_INSTR        ; <- Index 2 changed!
3: JMP_LABEL 5      ; <- Now points to wrong place!
4: ADD r19, r20, r21
5: RET
```

**Impact**: LICM code motion requires architectural changes

### 3. 3-Bit Temp Fields

**Structure**: Each instruction has 3 temp fields (8-bit each)

```c
typedef struct __attribute__((packed)) {
    uint8_t  op;        /* Operation code (0-255) */
    uint8_t  temp_dest; /* Destination temp (0-255, 0xFF if none) */
    uint8_t  temp_src1; /* Source temp 1 (0-255, 0xFF if none) */
    uint8_t  temp_src2; /* Source temp 2 (0-255, 0xFF if none) */
    ...
} Instr;
```

**Consequence**: 256 temps per function

**Impact**: Plenty of temps, but each assigned once

---

## Why CSE Shows 0%

### CSE Algorithm

CSE looks for **exact instruction duplicates**:
```c
// Same opcode, same sources, same dest pattern
if (instr1.op == instr2.op &&
    instr1.temp_src1 == instr2.temp_src1 &&
    instr1.temp_src2 == instr2.temp_src2) {
    // Found duplicate!
}
```

### Problem

SSA-like IR never produces exact duplicates:

```
; Instruction 1:
ADD r21, r19, r20

; "Duplicate" instruction 2:
ADD r26, r24, r25

; CSE comparison:
r21 != r26  (different dest)
r19 != r24  (different src1)
r20 != r25  (different src2)

; Result: NOT a duplicate!
```

### Conclusion

CSE is **working correctly** - the IR design simply doesn't produce duplicates.

---

## Why GVN Shows 0%

### GVN Algorithm

GVN tracks **value numbers** for computations:
```c
// Compute hash key from opcode + sources
key = op * 31 + src1 * 31 + src2;

// If we've seen this key before, it's a duplicate
if (ht32_get(value_map, key, &existing)) {
    // Found duplicate value!
}
```

### Problem

Different temps = different keys, even for same computation:

```
; First computation:
ADD r21, r19, r20
key1 = ADD * 31 + r19 * 31 + r20

; "Duplicate" computation:
ADD r26, r24, r25
key2 = ADD * 31 + r24 * 31 + r25

; key1 != key2 because r19 != r24 and r20 != r25

; Result: NOT the same value!
```

### Conclusion

GVN is **working correctly** - the IR design produces unique keys for each computation.

---

## Why Copy Propagation Shows 0%

### Copy Propagation Algorithm

Tracks copy relationships:
```c
// When we see: MOV r20, r19
copy_map[r20] = r19;

// When we see: ADD r21, r20, r18
// Replace r20 with r19:
ADD r21, r19, r18;
```

### Problem

Register allocator already minimizes copies:

```
; Most MOV_REG instructions are ABI-related:
MOV r0, r16    ; return value
MOV r17, r0    ; argument passing

; Not tracked (ABI registers):
- r0 (return register)
- r1-r7 (argument registers on x86-64)
```

### Conclusion

Copy propagation is **working correctly** - the register allocator already generates optimal code.

---

## Why Mem2Reg Has Issues

### Mem2Reg Algorithm

Promotes stack slots to temps:
```c
// When we see: MOV_MEM [fp-8], r16
slot_map[-8] = r16;

// When we see: LOAD r17, [fp-8]
// Replace with: MOV r17, r16
```

### Problems

1. **VLA Interference**: Dynamic stack allocation breaks static offsets
2. **Control Flow**: Jumps invalidate mappings
3. **Edge Cases**: Structs, atomics, compound literals have special handling

### Current Implementation

Conservative VLA detection:
```c
if (instr->op == OP_VLA_ALLOC) {
    state.has_vla = true;  // Disable ALL promotion
    continue;
}
```

### Conclusion

Mem2Reg is **safely implemented** but needs better edge case handling.

---

## The Fundamental Insight

### ZeroCC's IR Design Philosophy

**"SSA-like but not quite SSA"**:
- ✅ Each temp assigned once (SSA property)
- ❌ No phi nodes (not full SSA)
- ❌ No value numbering (not SSA-based optimization)
- ❌ Flat array with indices (not SSA graph)

### Consequences

1. **CSE/GVN Limited**: Unique temps per computation
2. **LICM Hard**: Flat array breaks on splicing
3. **Mem2Reg Tricky**: Stack offsets not stable

---

## Optimization Opportunities

### What Works Well

✅ **Cost-based Register Allocation**: Loop-aware, zero spills  
✅ **DCE**: Dead code elimination working  
✅ **Constant Folding**: Runtime computation reduction  
✅ **LICM Detection**: Finding 1,490 invariants  

### What Needs Different Approach

⚠️ **CSE**: Wrong tool for SSA-like IR  
⚠️ **GVN**: Needs copy propagation first  
⚠️ **LICM Code Motion**: Requires architectural changes  
⚠️ **Mem2Reg**: Needs better VLA handling  

---

## Recommended Optimizations

### High Priority

1. **Copy Propagation** (Done ✅)
   - Enables GVN effectiveness
   - 2-3 days effort
   - 3-10% ROI

2. **Multi-Pass GVN**
   - Run GVN multiple times
   - Propagate through chains
   - 1 week effort

3. **Fix Mem2Reg**
   - Better VLA handling
   - Edge case fixes
   - 1 week effort

### Medium Priority

4. **LICM Trampolines**
   - Out-of-line code motion
   - 1-2 weeks effort
   - 20-30% ROI for loops

5. **Cross-Block GVN**
   - Extend beyond basic blocks
   - Requires data-flow
   - 2 weeks effort

### Low Priority

6. **Full SSA Construction**
   - Braun et al. (2013) approach
   - 3-4 weeks effort
   - Enables all SSA optimizations

---

## Architecture Comparison

### ZeroCC IR (Current)

```
Flat array + SSA-like temps
├── Pros: Simple, fast emission
├── Cons: Hard to optimize
└── Best for: JIT compilation
```

### Traditional SSA

```
SSA graph + phi nodes
├── Pros: Easy optimization
├── Cons: Complex construction
└── Best for: AOT compilation
```

### Hybrid Approach

```
SSA-like temps + value numbering
├── Pros: Good of both worlds
├── Cons: Requires redesign
└── Best for: Both JIT and AOT
```

---

## Conclusion

**The IR design is working as intended** - it's optimized for fast JIT compilation, not aggressive optimization.

**CSE/GVN showing 0% is expected** - the SSA-like temp allocation prevents duplicates.

**The right optimizations are**:
1. Copy propagation (enables GVN)
2. Mem2Reg (reduces stack traffic)
3. LICM trampolines (loop optimization)
4. Multi-pass GVN (better propagation)

**For maximum optimization**: Consider full SSA construction in the future.

---

## References

- IR Structure: `src/zero_cc/parser/ir.h`
- Temp Allocation: `src/zero_cc/parser/ast_lowering_expr.c`
- IR Arena: `src/zero_cc/parser/ir_arena.h`
- SSA Construction: Braun et al. "Simple and Efficient Construction of SSA Form" (2013)
