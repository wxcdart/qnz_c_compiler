# How to Read the Prologue/Epilogue Fragility Documentation

This directory contains 3 documents explaining why the prologue/epilogue is fragile and how to fix it.

## Quick Navigation

### If you have 5 minutes
→ Read `PROLOGUE_EPILOGUE_SUMMARY.md`
- Executive summary of the problem
- Real example of a bug that occurred
- Why it matters

### If you have 30 minutes
→ Read `docs/PROLOGUE_EPILOGUE_ANALYSIS.md`
- Detailed analysis of all 5 fragile synchronization points
- Root causes with code examples
- Why fixes are needed

### If you have 1-2 hours
→ Read `docs/PROLOGUE_EPILOGUE_IMPROVEMENT_PLAN.md`
- Concrete 4-phase improvement plan
- Code diffs for each phase
- Timeline and risk assessment
- Testing strategy

## The Problem in One Sentence

**Prologue/epilogue logic is spread across 7+ files with no single authority, no validation, and happens in two disconnected stages (IR emit + IR patch), making it easy to introduce stack misalignment bugs.**

## The Bug That Happened

1. I changed frame size calculation to use 8 mod 16 alignment
2. Didn't realize this was incompatible with System V AMD64 ABI (requires 16-byte alignment)
3. Assembly emitter used the wrong frame size: `sub $72, %rsp` (72 % 16 = 8, not 0!)
4. Stack misalignment caused segfault when calling printf()
5. Tests only catch this if they call library functions that require correct alignment

## The Root Cause

Five disconnected synchronization points that must all stay in sync:

```
Frame size calculation  → +15 & ~15
    ↓
IR emission            → OP_PROLOGUE with placeholder
    ↓
Register allocation    → might add spill slots
    ↓
Frame size patch       → overwrite OP_PROLOGUE.imm64
    ↓
Machine code lowering  → read OP_PROLOGUE.imm64 → emit prologue
    ↓
Assembly lowering      → SEPARATE CODE PATH, read OP_PROLOGUE.imm64 → emit prologue
```

If alignment is wrong in ONE place, both paths fail silently.

## Why This Matters

Prologue/epilogue is **low-level, high-impact**:
- Called at entry/exit of EVERY function
- Must get stack alignment exactly right
- Requires understanding of System V ABI
- Easy to introduce subtle bugs
- Hard to debug (manifests as "random" segfaults)

## The Solution

Make frame layout calculation:
1. **Centralized**: Single `calculate_frame_layout()` function
2. **Early**: Before IR is emitted, not after
3. **Validated**: Check alignment, size, all invariants
4. **Immutable**: Never patched or overwritten

## Reading Order Recommendation

**For Contributors**:
1. Read PROLOGUE_EPILOGUE_SUMMARY.md (understand the problem)
2. Read docs/PROLOGUE_EPILOGUE_ANALYSIS.md (understand root causes)
3. Read docs/PROLOGUE_EPILOGUE_IMPROVEMENT_PLAN.md (when implementing fixes)

**For Code Reviewers**:
1. Read PROLOGUE_EPILOGUE_SUMMARY.md
2. Look at specific fragile points in docs/PROLOGUE_EPILOGUE_ANALYSIS.md
3. Check if changes break any invariants

**For Bug Hunters**:
1. Read PROLOGUE_EPILOGUE_ANALYSIS.md (look for similar patterns)
2. Check the 5 synchronization points
3. Add validation from PROLOGUE_EPILOGUE_IMPROVEMENT_PLAN.md Phase 1

## Key Files to Understand

| File | Purpose | Why Fragile |
|------|---------|------------|
| `src/zero_cc/parser/ast_lowering.c:115-135` | Frame size calculation | Multiple calculations, magic constants, no validation |
| `src/zero_cc/parser/ir.h:427` | Prologue IR emission | Placeholder size, overwritten later |
| `src/zero_cc/parser/ast_lowering_mc.c` | Machine code lowering | Dispatches to backend, no validation |
| `src/zero_cc/parser/asm_emitter.c` | Assembly emission | Separate code path, can diverge |
| `src/zero_cc/isa/x86_64.c` | x86_64 prologue | Hardcoded bytes, easy to get wrong |

## The Alignment Bug in Detail

**What went wrong**:
```c
// WRONG: This makes frame_size = 8 mod 16
size_t base_size = (c->current_stack_frame + 7) & ~7;
total_frame_size = base_size + 8;  // Adds 8 to already 8-aligned value
```

**Why it's wrong**:
- System V AMD64 ABI requires RSP = 0 mod 16 before any CALL
- After `push rbp` (8 bytes): RSP = 0 mod 16
- After `sub rsp, frame_size`: RSP = (0 - frame_size) mod 16
- For RSP to stay 0 mod 16, frame_size must be 0 mod 16
- But my code made frame_size = 8 mod 16!

**What the segfault looked like**:
```
./test_printf: line 1: 958121 Segmentation fault (core dumped)
```

The segfault happened inside printf because the stack was misaligned.

## Testing After Fixes

Run these to verify fixes don't break anything:
```bash
make test      # All 118/118 tests should pass
make aot       # AOT compilation tests
./build/bin/zerocc -S -o /tmp/test.s test.c  # Assembly generation
```

## Questions?

- **"Why is prologue/epilogue so complicated?"** → Read PROLOGUE_EPILOGUE_ANALYSIS.md section "Root Causes of Fragility"
- **"How do I fix this?"** → Read PROLOGUE_EPILOGUE_IMPROVEMENT_PLAN.md
- **"What was the bug?"** → Read PROLOGUE_EPILOGUE_SUMMARY.md section "A Real Bug"
- **"Why didn't tests catch this?"** → Because JIT tests don't call library functions that require correct stack alignment

## Quick Reference

**Frame size must be**:
- ✅ 16-byte aligned (0 mod 16)
- ✅ Positive (> 0)
- ✅ Less than 16MB
- ✅ Consistent across IR and machine code
- ✅ Validated before use

**Prologue sequence must be**:
- ✅ First instruction (IR index 0)
- ✅ Push RBP
- ✅ Mov RSP, RBP
- ✅ Sub RSP, frame_size (if frame_size > 0)

**Common mistakes**:
- ❌ Mixing 8-byte and 16-byte alignment
- ❌ Calculating frame size multiple times
- ❌ Not validating alignment
- ❌ Assuming prologue is at index 0 without checking
- ❌ Not syncing machine code and assembly emitters

