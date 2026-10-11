# Prologue/Epilogue Fragility Analysis - Complete Index

## 📚 Documentation Overview

This directory now contains comprehensive analysis of why prologue/epilogue handling is fragile, a real bug that occurred, and how to fix it.

## 🚀 Quick Start

**New here? Start with README_FRAGILITY_DOCS.md** (5-10 minutes)
- Provides navigation based on available time
- Quick one-sentence problem summary
- FAQ index directing you to specific sections

## 📖 Documentation Files

### Entry Points (Pick One Based on Time Available)

| Time | Document | Purpose |
|------|----------|---------|
| **5 min** | `README_FRAGILITY_DOCS.md` | Quick navigation, one-sentence problem, FAQ |
| **10-15 min** | `FRAGILITY_ANALYSIS_VISUAL_SUMMARY.txt` | Visual tree diagrams, key findings, solution |
| **15-20 min** | `PROLOGUE_EPILOGUE_SUMMARY.md` | Executive summary, real bugs, improvement plan |
| **30-60 min** | `docs/PROLOGUE_EPILOGUE_ANALYSIS.md` | Deep technical analysis, root causes |
| **2+ hours** | `docs/PROLOGUE_EPILOGUE_IMPROVEMENT_PLAN.md` | Implementation roadmap with code examples |

### Reference Documents

| Document | Size | Content |
|----------|------|---------|
| `FRAGILITY_ANALYSIS_COMPLETE.md` | 6.6K | Summary of all analysis and next steps |
| `FRAGILITY_ANALYSIS_VISUAL_SUMMARY.txt` | 7.4K | Visual summary with ASCII trees |
| `INDEX_FRAGILITY_ANALYSIS.md` | This file | Index and navigation guide |

## 🎯 What This Analysis Covers

### The Problem
- Prologue/epilogue logic spread across 7+ files
- Two disconnected stages (IR emit, then patch)
- Multiple frame size calculations with no validation
- Assembly and machine code using separate implementations

### The Bug
- Stack misalignment bug in assembly emitter
- Caused by incorrect frame size calculation
- Manifested as segfault when calling printf()
- Wasn't caught by existing tests

### The Root Causes
- **5 fragile synchronization points** that can get out of sync
- **Two-phase frame size** (emit placeholder, patch later)
- **No validation** of alignment or size
- **Distributed responsibility** across multiple files

### The Solution
- **Centralized** frame layout calculation
- **Early** calculation (before IR emission)
- **Validated** at multiple points
- **Immutable** frame size (no patching)

### The Improvement Plan
- **Phase 1**: Add validation (2-4 hours, very low risk)
- **Phase 2**: Centralize calculation (2-3 days, low risk)
- **Phase 3**: Add assertions (1 day, very low risk)
- **Phase 4**: ISA-agnostic template (3-5 days, medium risk)

## 🔍 The Five Fragile Synchronization Points

1. **Frame size calculated LATE, patched RETROACTIVELY**
   - No validation of alignment
   - Silent failure if assumptions wrong

2. **Prologue ASSUMED to be FIRST IR instruction**
   - Would break if IR structure changes
   - No assertion to catch this

3. **MULTIPLE frame size CALCULATIONS**
   - 5 calculations, 4 variables, scattered magic constants
   - Easy to miss one when making changes

4. **Assembly has SEPARATE prologue code**
   - Two implementations that can diverge
   - No consistency checks

5. **Callee-save handling UNCLEAR**
   - "Separate IR instructions" don't sync with frame size
   - Works by accident

## 🐛 The Bug Explained

```
Stage 1: I changed frame size calculation to 8 mod 16 alignment
         ↓
Stage 2: Assembly output: sub $72, %rsp (72 % 16 = 8, not 0!)
         ↓
Stage 3: Stack misalignment when calling printf()
         ↓
Stage 4: Segfault, but only caught because printf needs alignment
```

**Why tests didn't catch it initially**:
- JIT tests only test internal functions
- Assembly emitter not in automated test suite
- Stack misalignment only manifests with library calls

## 🛠️ How to Use This Documentation

### If you're a...

**Contributor fixing a prologue/epilogue bug**:
1. Start: `README_FRAGILITY_DOCS.md`
2. Read: `docs/PROLOGUE_EPILOGUE_ANALYSIS.md`
3. Implement: `docs/PROLOGUE_EPILOGUE_IMPROVEMENT_PLAN.md`

**Code reviewer checking prologue/epilogue changes**:
1. Skim: `FRAGILITY_ANALYSIS_VISUAL_SUMMARY.txt`
2. Check: Section on "5 Fragile Synchronization Points"
3. Verify: No similar fragility introduced

**Designer planning compiler architecture**:
1. Read: `docs/PROLOGUE_EPILOGUE_ANALYSIS.md`
2. Study: "Root Causes of Fragility" section
3. Adopt: "Solution Pattern" approach

**Debugger hunting for prologue/epilogue bugs**:
1. Check: 5 synchronization points
2. Look for: Similar patterns elsewhere in codebase
3. Add: Validation from Phase 1 of improvement plan

### By Topic

**Understanding the fragility**:
- `README_FRAGILITY_DOCS.md` - Quick overview
- `PROLOGUE_EPILOGUE_SUMMARY.md` - Problem and solution
- `docs/PROLOGUE_EPILOGUE_ANALYSIS.md` - Deep analysis

**Implementing fixes**:
- `docs/PROLOGUE_EPILOGUE_IMPROVEMENT_PLAN.md` - Phased approach with code
- `FRAGILITY_ANALYSIS_COMPLETE.md` - Next steps for maintainers

**Understanding the specific bug**:
- `FRAGILITY_ANALYSIS_VISUAL_SUMMARY.txt` - "The Bug That Happened" section
- `README_FRAGILITY_DOCS.md` - "The Alignment Bug in Detail" section

## 📊 Files Modified

**Bugs Fixed**:
- `src/zero_cc/parser/ast_lowering.c` - Removed buggy conditional alignment

**Documentation Created**:
- `README_FRAGILITY_DOCS.md` - Navigation guide
- `PROLOGUE_EPILOGUE_SUMMARY.md` - Executive summary
- `docs/PROLOGUE_EPILOGUE_ANALYSIS.md` - Deep technical analysis
- `docs/PROLOGUE_EPILOGUE_IMPROVEMENT_PLAN.md` - Implementation roadmap
- `FRAGILITY_ANALYSIS_COMPLETE.md` - Summary and next steps
- `FRAGILITY_ANALYSIS_VISUAL_SUMMARY.txt` - Visual summary
- `INDEX_FRAGILITY_ANALYSIS.md` - This file

## ✅ Test Results

- **All 118/118 tests PASSING** ✅
- No regressions from bug fix
- Frame size now consistent across JIT and assembly
- Stack alignment correct (16-byte)

## 📝 Commits

| Hash | Message |
|------|---------|
| `65ffac1` | Fix: assembly emitter stack alignment |
| `2721cad` | Docs: prologue/epilogue analysis and improvement plan |
| `ee0cf49` | Docs: executive summary of prologue/epilogue fragility |
| `d4efbdc` | Docs: guide for reading prologue/epilogue fragility documentation |
| `749c17a` | Docs: complete fragility analysis summary |
| `72eae1c` | Docs: visual summary of fragility analysis |

## 🔗 Related Code

**Key files to understand**:
- `src/zero_cc/parser/ast_lowering.c:115-135` - Frame size calculation
- `src/zero_cc/parser/ir.h:427` - Prologue IR emission
- `src/zero_cc/parser/ast_lowering_mc.c` - Machine code lowering
- `src/zero_cc/parser/asm_emitter.c` - Assembly emission
- `src/zero_cc/isa/x86_64.c` - x86_64 prologue emission

## 💡 Key Insights

1. **Multi-stage process needs consistency enforcement**
   - Prologue/epilogue involves parse → IR → lower → emit
   - Current design provides no mechanism to enforce consistency

2. **Two-phase frame size is fundamentally fragile**
   - Emit placeholder before register allocation
   - Patch after register allocation
   - No synchronization mechanism

3. **Stack alignment is critical and easy to get wrong**
   - System V ABI requires 16-byte alignment
   - One byte off causes segfaults
   - Should be validated at multiple points

4. **Code duplication leads to divergence**
   - Assembly and machine code have separate prologue implementations
   - Easy to forget to update both
   - Should use common abstraction

## 🎓 Learning Outcomes

After reading this analysis, you should understand:
- ✅ Why prologue/epilogue is fragile
- ✅ The 5 synchronization points that can fail
- ✅ The specific bug that occurred and why
- ✅ Why existing tests didn't catch it
- ✅ The solution pattern to fix it
- ✅ The phased approach to implementation
- ✅ How to avoid similar fragility in other code

## 📞 Questions?

Common questions and answers:

**Q: Why is prologue/epilogue so complicated?**
A: See `docs/PROLOGUE_EPILOGUE_ANALYSIS.md` section "Root Causes of Fragility"

**Q: How do I fix this?**
A: See `docs/PROLOGUE_EPILOGUE_IMPROVEMENT_PLAN.md` for 4-phase approach

**Q: What was the actual bug?**
A: See `PROLOGUE_EPILOGUE_SUMMARY.md` section "A Real Bug" or `FRAGILITY_ANALYSIS_VISUAL_SUMMARY.txt`

**Q: Why didn't tests catch this?**
A: See `README_FRAGILITY_DOCS.md` FAQ section

**Q: What should I do next?**
A: See `FRAGILITY_ANALYSIS_COMPLETE.md` section "Next Steps for Maintainers"

## 🚀 Recommended Next Steps

### Immediate (Important for Code Quality)
- [ ] **Phase 1**: Add prologue validation function (2-4 hours)
- [ ] **Phase 3**: Add assertions in lowering (1 day)

### Short-term (1-2 weeks)
- [ ] **Phase 2**: Centralize frame size calculation (2-3 days)

### Medium-term (Nice to Have)
- [ ] **Phase 4**: ISA-agnostic prologue template (3-5 days)

---

**Last updated**: October 8, 2024
**Status**: Analysis complete, all tests passing, recommendations provided
**Next action**: Read README_FRAGILITY_DOCS.md or FRAGILITY_ANALYSIS_VISUAL_SUMMARY.txt
