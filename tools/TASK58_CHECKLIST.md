# Task 58: Loop & Control-Flow Optimizations Checklist

## Overview
**Task 58**: Loop-invariant code motion (LICM), strength reduction, and branch simplification
**Timeline**: ~5 days
**Status**: 📅 Ready to start

## Pre-Work Complete ✅

### Infrastructure Verification
- [x] deps_graph.json updated (101 files, 1460 functions)
- [x] No circular dependencies found
- [x] Optimization pass infrastructure verified
- [x] IR opcode infrastructure verified
- [x] Register allocator available
- [x] Loop constructs verified

### Tools Created
- [x] `tools/task58_precheck.py` - Readiness check
- [x] `tools/task58_analysis.py` - Requirements analysis
- [x] `tools/check_circular_deps.py` - Circular dependency checker

## Implementation Phases

### Phase 1: Loop Analysis Framework (1 day)

#### Files to Create
- [ ] `src/zero_cc/parser/opt_loop.h` (50 lines)
  - Loop structure definitions
  - Dominator tree API
  - Loop header/backedge detection

- [ ] `src/zero_cc/parser/opt_loop.c` (200 lines)
  - Loop detection algorithm
  - Dominator tree construction
  - Loop preheader identification

#### Key Functions
- [ ] `loop_detect()` - Find all loops in IR
- [ ] `loop_build_dominators()` - Build dominator tree
- [ ] `loop_find_header()` - Identify loop header
- [ ] `loop_find_backedges()` - Find backedges
- [ ] `loop_is_invariant()` - Check if expression is loop-invariant

### Phase 2: Loop-Invariant Code Motion (1.5 days)

#### Files to Modify
- [ ] `src/zero_cc/parser/opt_loop.c` - Add LICM implementation

#### Key Functions
- [ ] `licm_detect_invariants()` - Find loop-invariant expressions
- [ ] `licm_move_to_preheader()` - Move invariants out of loop
- [ ] `licm_has_side_effects()` - Check if instruction has side effects
- [ ] `licm_is_safe_to_move()` - Verify move is safe

#### IR Changes
- [ ] Handle `OP_LABEL` as loop preheader
- [ ] Handle `OP_JMP`, `OP_JE`, `OP_JNE` as control flow
- [ ] Preserve instruction ordering

### Phase 3: Strength Reduction (1 day)

#### Files to Modify
- [ ] `src/zero_cc/parser/opt_loop.c` - Add strength reduction

#### Key Functions
- [ ] `sr_detect_multiplications()` - Find `i * constant` patterns
- [ ] `sr_is_power_of_2()` - Check if constant is power of 2
- [ ] `sr_replace_with_shift()` - Replace mul with shift
- [ ] `sr_replace_with_add()` - Replace mul with add chain

#### Transformations
- [ ] `x = i * 2` → `x = i << 1`
- [ ] `x = i * 4` → `x = i << 2`
- [ ] `x = i * 8` → `x = i << 3`
- [ ] `x = i * 16` → `x = i << 4`
- [ ] `x = i * 3` → `x = (i << 1) + i`

### Phase 4: Branch Simplification (0.5 days)

#### Files to Modify
- [ ] `src/zero_cc/parser/opt_dce.c` - Add branch simplification

#### Key Functions
- [ ] `bs_simplify_constant_branch()` - Remove dead branches
- [ ] `bs_simplify_infinite_loop()` - Optimize `while(1)`
- [ ] `bs_remove_unreachable()` - Remove unreachable code

#### Transformations
- [ ] `if (true) { A }` → `A`
- [ ] `if (false) { A } else { B }` → `B`
- [ ] `while (true) { if (c) break; }` - Keep as-is (common pattern)

### Phase 5: Testing & Integration (1 day)

#### Integration
- [ ] Add LICM to `opt_pass.c` pipeline
- [ ] Add strength reduction to `opt_pass.c` pipeline
- [ ] Add branch simplification to `opt_pass.c` pipeline
- [ ] Update Makefile if needed

#### Testing
- [ ] Run full test suite (118 tests)
- [ ] Verify LICM correctness
- [ ] Verify strength reduction correctness
- [ ] Verify branch simplification correctness
- [ ] Benchmark performance improvement

#### Documentation
- [ ] Update `CHANGELOG.md`
- [ ] Update `ZEROCC_Development_Plan.md`
- [ ] Commit with detailed message

## Success Criteria

- [ ] All 118 tests passing
- [ ] LICM correctly moves loop-invariant code
- [ ] Strength reduction replaces multiplications
- [ ] Branch simplification removes dead code
- [ ] No performance regressions
- [ ] Documentation updated

## Dependencies

### Required from Task 57
- [x] Optimization pass framework (`opt_pass.h/c`)
- [x] Constant folding (`opt_constant_fold.h/c`)
- [x] Dead code elimination (`opt_dce.h/c`)
- [x] Common subexpression elimination (`opt_cse.h/c`)
- [x] IR infrastructure (`ir.h/c`)
- [x] Register allocator (`regalloc.h`)

### None
Task 58 builds directly on Task 57 infrastructure.

## Risks & Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| LICM breaks side effects | High | Careful side-effect analysis |
| Strength reduction incorrect | Medium | Test all power-of-2 cases |
| Dominator tree complexity | Medium | Start with simple algorithm |
| Performance regression | Low | Benchmark before/after |

## Notes

- LICM should run after constant folding and DCE
- Strength reduction should run after LICM
- Branch simplification can run anytime
- All passes should be idempotent (safe to run multiple times)

---

*Last updated: 2024-10-04*
*Ready to start: Yes*
