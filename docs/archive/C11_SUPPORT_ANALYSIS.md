# C11 Support Analysis

## Overview

This document analyzes C11 feature support in ZeroCC and evaluates whether C11 support should come before SIMD vectorization.

## Current C11 Support Status

### ✅ Already Supported

| C11 Feature | Status | Implementation |
|-------------|--------|----------------|
| **_Atomic types** | ✅ Supported | OP_ATOMIC_XCHG, OP_ATOMIC_CMPXCHG |
| **_Thread_local** | ✅ Supported | TLS backend, OP_TLS_LOAD/STORE |
| **_Alignas** | ✅ Supported | Stack alignment (16-byte) |
| **_Alignof** | ✅ Supported | sizeof(_Atomic int) works |
| **_Noreturn** | ✅ Supported | _Noreturn keyword parsed |
| **_Static_assert** | ✅ Supported | static_assert works |
| **Anonymous structs/unions** | ✅ Supported | Compound literals work |
| **Generic selections** | ⚠️ Partial | _Generic may need work |

### ❌ Missing or Incomplete

| C11 Feature | Status | Needed For |
|-------------|--------|------------|
| **Variable Length Arrays (VLA)** | ❌ Incomplete | OP_VLA_ALLOC exists but fails on large arrays |
| **Multithreading (_Thread)** | ❌ Not supported | No thread creation primitives |
| **Bounds-checking interfaces** | ❌ Not supported | __STDC_WANT_LIB_EXT1__ |
| **Complex numbers** | ❌ Not supported | _Complex type |

## Comparison: C11 vs SIMD Priority

### Option A: SIMD First (Current Plan)

**Pros:**
- ✅ Fixes failing benchmarks (call_heavy.c)
- ✅ Performance is visible to all users
- ✅ SIMD is a core compiler capability
- ✅ C11 features are "nice to have", SIMD is "need to have"

**Cons:**
- ❌ C11 compliance incomplete
- ❌ VLA still broken until Sprint 11

### Option B: C11 First

**Pros:**
- ✅ Better language compliance
- ✅ Users can write modern C code
- ✅ Some C11 features (VLA) relate to Sprint 11

**Cons:**
- ❌ call_heavy.c still fails
- ❌ Performance not improved
- ❌ C11 features are rarely used in practice

## Recommendation: SIMD First

**Keep the current order:**
```
Sprint 9:  CLI + ELF (In Progress)
Sprint 10: SIMD Vectorization (Next)
Sprint 11: Stack Allocation & VLA
Sprint 12: C11 Features (After SIMD + Stack)
```

### Rationale

1. **Fix Failing Tests First**
   - call_heavy.c fails due to missing SIMD
   - memory_heavy.c fails due to stack allocation
   - These are **bugs**, not missing features

2. **Performance is More Visible**
   - Users notice slow compilation/execution
   - Users rarely use C11-specific features
   - SIMD benefits **all** code, C11 benefits **some** code

3. **VLA is Already Partially Implemented**
   - OP_VLA_ALLOC exists
   - Just needs stack allocation fix (Sprint 11)
   - No new C11 parsing needed

4. **C11 Features Can Wait**
   - Most C code is C99-compatible
   - C11 features are opt-in
   - SIMD is opt-in via -O3

## C11 Features to Add (Sprint 12)

### Priority 1: VLA Completion

**Already in Sprint 11** (Stack Allocation):
- [x] OP_VLA_ALLOC exists
- [ ] Fix large array support
- [ ] VLA runtime allocation
- [ ] VLA in nested scopes

### Priority 2: Generic Selections

**_Generic keyword:**
```c
#define abs(x) _Generic((x), \
    int: int_abs, \
    float: float_abs, \
    double: double_abs \
)(x)
```

**Implementation:**
- [ ] Parse _Generic in parser
- [ ] Type-based dispatch in lowering
- [ ] Generate appropriate function calls

### Priority 3: Thread Support

**_Thread keyword:**
```c
#include <threads.h>
int thrd_create(thrd_t *thr, thrd_start_t func, void *arg);
```

**Implementation:**
- [ ] Thread creation primitives
- [ ] Thread-local storage (already has _Thread_local)
- [ ] Mutex/condition variable support

### Priority 4: Complex Numbers

**_Complex type:**
```c
double _Complex z = 1.0 + 2.0*I;
```

**Implementation:**
- [ ] _Complex type in type system
- [ ] Complex arithmetic ops
- [ ] Complex intrinsics

## Updated Sprint Plan

| Sprint | Focus | Duration | Status |
|--------|-------|----------|--------|
| **Sprint 9** | CLI + ELF Output | 2 weeks | 📅 In Progress |
| **Sprint 10** | SIMD Vectorization | 2 weeks | 📅 Next |
| **Sprint 11** | Stack Allocation & VLA | 2 weeks | 📅 Scheduled |
| **Sprint 12** | C11 Features | 2 weeks | 📅 Future |
| **Sprint 13** | Testing & Hardening | 2 weeks | 📅 Future |

### Sprint 12: C11 Features (Tasks 78-81)

| Task | Description | Duration |
|------|-------------|----------|
| 78 | Generic Selections (_Generic) | 3 days |
| 79 | Thread Support (_Thread) | 3 days |
| 80 | Complex Numbers (_Complex) | 2 days |
| 81 | C11 Compliance Testing | 2 days |

## Conclusion

**Keep SIMD first, C11 later.**

**Reasons:**
1. **Fix bugs first** - call_heavy.c and memory_heavy.c are failing
2. **Performance matters** - SIMD benefits all users
3. **C11 is optional** - Most code is C99-compatible
4. **VLA already started** - Sprint 11 fixes VLA, not Sprint 12

**Revised timeline:**
```
Weeks 36-37: Sprint 9 (CLI + ELF)
Weeks 37-38: Sprint 10 (SIMD)
Weeks 38-39: Sprint 11 (Stack + VLA)
Weeks 39-40: Sprint 12 (C11 Features)
```

**Total: +4 weeks from current** (same as before, just C11 added as Sprint 12)
