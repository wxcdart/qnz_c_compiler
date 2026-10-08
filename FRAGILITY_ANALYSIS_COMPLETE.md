# Prologue/Epilogue Fragility Analysis - Complete

## What We Learned

Through investigating a stack alignment bug in the assembly emitter, we discovered that **prologue/epilogue handling is one of the most fragile parts of the ZeroCC compiler**.

## The Deliverables

Created comprehensive documentation explaining the fragility:

### 1. README_FRAGILITY_DOCS.md
**Entry point for developers**
- Quick navigation by available time
- Problem summary in one sentence
- Reading recommendations by role
- Quick reference guide
- FAQ index

### 2. PROLOGUE_EPILOGUE_SUMMARY.md
**Executive summary** (5-10 minutes)
- The core problem (5 disconnected systems)
- The 5 fragile synchronization points
- A real bug that occurred (stack misalignment)
- Why it happened (lack of validation)
- The ideal design pattern
- Concrete 4-phase improvement plan

### 3. docs/PROLOGUE_EPILOGUE_ANALYSIS.md
**Deep technical analysis** (30-60 minutes)
- Detailed analysis of each fragile point with code
- Root causes of fragility
- Real bugs that have occurred
- Lessons learned
- Recommended fixes with specific examples
- Testing strategy

### 4. docs/PROLOGUE_EPILOGUE_IMPROVEMENT_PLAN.md
**Implementation roadmap** (1-2 hours)
- 4-phase improvement plan:
  - Phase 1: Validation (2-4 hours, very low risk)
  - Phase 2: Centralize (2-3 days, low risk)
  - Phase 3: Consistency checks (1 day, very low risk)
  - Phase 4: ISA-agnostic (3-5 days, medium risk)
- Concrete code examples for each phase
- Risk assessment per phase
- Testing plan
- Timeline and success criteria

## The Problem Explained

**In one sentence**: Prologue/epilogue logic is spread across 7+ files in two disconnected stages (IR emit + IR patch) with no validation, making stack misalignment bugs easy to introduce.

**The five fragile points**:
1. Frame size calculated late, patched retroactively (no validation)
2. Prologue assumed to be first IR instruction (no assertion)
3. Multiple frame size calculations (easy to miss one)
4. Assembly has separate prologue code (can diverge)
5. Callee-save handling is "separate IR instructions" (unclear sync)

**Why it matters**: 
- Prologue/epilogue called at entry/exit of EVERY function
- Must get stack alignment exactly right (System V ABI requires 16-byte)
- One byte off causes segfaults in library calls
- Hard to debug (manifests as random segfaults)

## The Bug That Happened

1. Changed frame size calculation to 8 mod 16 alignment
2. Didn't realize this violated System V AMD64 ABI (requires 16-byte)
3. Assembly output: `sub $72, %rsp` (72 % 16 = 8, not 0!)
4. Stack misalignment caused segfault when calling printf()
5. Tests only caught this because printf requires correct alignment

## The Solution

Make frame layout calculation:
- **Centralized**: Single `calculate_frame_layout()` function
- **Early**: Before IR is emitted, not after register allocation
- **Validated**: Check alignment, size, all invariants
- **Immutable**: Never patched or overwritten

## Key Insights

### The Core Issue
Prologue/epilogue is fundamentally a **multi-stage process** (parse → IR → lower → emit), and the current design doesn't enforce consistency across stages.

### Why Tests Didn't Catch It
- JIT tests only test internal functions
- Assembly emitter not tested in automated tests
- Stack misalignment only manifests when calling functions that require correct alignment
- Would have been caught by tests that call library functions (printf, printf, etc.)

### The Design Anti-Pattern
Using **two-phase frame size** (emit placeholder, patch later) is fragile:
- Prologue emitted before register allocation
- Frame size not known until after register allocation
- Frame size patched into IR retroactively
- No synchronization mechanism

### The Better Pattern
Calculate frame layout **before** IR emission:
- Know all stack requirements
- Emit correct frame size on first try
- Validate immediately
- No patching needed

## Quick Reference: What's Fragile

| Component | Fragility | Why |
|-----------|-----------|-----|
| Frame size calculation | ⚠️⚠️⚠️ | Multiple calculations, magic constants, no validation |
| Prologue IR emission | ⚠️⚠️ | Placeholder size, overwritten later |
| Frame size patching | ⚠️⚠️⚠️ | Assumes IR index 0 is prologue, no error handling |
| Machine code emission | ⚠️ | Trusts frame size, no validation |
| Assembly emission | ⚠️⚠️ | Separate code path, can diverge from machine code |
| Backend prologue | ⚠️⚠️ | Hardcoded bytes, easy to get wrong |

## Files Modified

**Documentation Created**:
- `README_FRAGILITY_DOCS.md` - Quick navigation guide
- `PROLOGUE_EPILOGUE_SUMMARY.md` - Executive summary
- `docs/PROLOGUE_EPILOGUE_ANALYSIS.md` - Deep technical analysis
- `docs/PROLOGUE_EPILOGUE_IMPROVEMENT_PLAN.md` - Implementation roadmap
- `FRAGILITY_ANALYSIS_COMPLETE.md` - This file

**Code Fixed**:
- `src/zero_cc/parser/ast_lowering.c` - Frame size calculation (removed buggy conditional)
- Test results: **118/118 PASS** ✅

## Next Steps for Maintainers

### Immediate (Important for Code Quality)
1. **Phase 1**: Add prologue validation function (2-4 hours)
   - Catches misconfigurations early
   - Very low risk, high value
   - Recommended to do this now

2. **Phase 3**: Add assertions in lowering (1 day)
   - Prevents silent failures
   - Very low risk, catches divergence

### Short Term (1-2 weeks)
3. **Phase 2**: Centralize frame size calculation (2-3 days)
   - Single source of truth
   - Better code organization

### Medium Term (Nice to Have)
4. **Phase 4**: ISA-agnostic prologue template (3-5 days)
   - Prevents x86-64 assumptions creeping into generic code
   - Fixes similar issues for other ISAs automatically

## Recommended Reading Path

**5 minutes**: README_FRAGILITY_DOCS.md
**30 minutes**: PROLOGUE_EPILOGUE_SUMMARY.md
**60 minutes**: docs/PROLOGUE_EPILOGUE_ANALYSIS.md
**2+ hours**: docs/PROLOGUE_EPILOGUE_IMPROVEMENT_PLAN.md

## Test Results

All 118/118 tests passing ✅
- No regressions from bug fix
- Frame size now consistent across JIT and assembly
- Stack alignment correct (16-byte)

## Commits

- `65ffac1` - Fix: assembly emitter stack alignment
- `2721cad` - Docs: prologue/epilogue analysis and improvement plan
- `ee0cf49` - Docs: executive summary
- `d4efbdc` - Docs: reading guide

## Key Takeaway

**Prologue/epilogue is fragile because it involves multiple disconnected systems that must stay perfectly synchronized. The solution is to centralize the calculation, validate early, and keep frame size immutable once determined.**

This analysis provides the foundation for making this critical part of the compiler significantly more robust.
