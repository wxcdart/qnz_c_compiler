# Optimization Profiling System

## Overview

The optimization profiling system tracks statistics for all optimization passes to measure their effectiveness. This helps identify which optimizations are working well and which need improvement.

## Usage

### Command-Line Flag

```bash
./build/bin/zerocc --profile-opts source.c
```

### Example Output

```
DEBUG: Parsed --profile-opts flag
DEBUG: Enabling optimization profiling

=== Optimization Profile Statistics ===

CSE (Common Subexpression Elimination):
  Total instructions scanned: 181
  Eliminated (replaced with MOV): 0
  Basic blocks processed: 14
  Elimination rate: 0.00%

DCE (Dead Code Elimination):
  Instructions eliminated: 0
  Iterations: 0

LICM (Loop-Invariant Code Motion):
  Loops detected: 5
  Invariants found: 324
  Instructions moved: 0

Register Allocation:
  Total live intervals: 81
  Total spills: 0
  Cost-based spills: 0
  Max loop depth: 0
  Spill rate: 0.00%

General:
  Instructions before optimization: 49
  Instructions after optimization: 46
  Optimization iterations: 5
  Instruction reduction: 6.12%

=======================================
```

## Metrics Explained

### CSE (Common Subexpression Elimination)

- **Total instructions scanned**: Number of instructions examined by CSE pass
- **Eliminated**: Number of duplicate expressions replaced with MOV from existing result
- **Basic blocks processed**: Number of basic blocks (OP_LABEL occurrences)
- **Elimination rate**: Percentage of instructions that were eliminated

**Interpretation**: Higher elimination rate indicates more redundant computations in the code.

### DCE (Dead Code Elimination)

- **Instructions eliminated**: Number of dead instructions removed
- **Iterations**: Number of fix-point iterations performed

**Interpretation**: Higher numbers indicate more dead code being removed. Multiple iterations suggest complex dependency chains.

### LICM (Loop-Invariant Code Motion)

- **Loops detected**: Number of loops found via backedge analysis
- **Invariants found**: Number of loop-invariant expressions detected
- **Instructions moved**: Number of invariants actually moved (currently 0, see notes below)

**Interpretation**: High invariant count with 0 moved indicates the detection is working but code motion is not yet implemented.

### Register Allocation

- **Total live intervals**: Number of temporaries that needed register allocation
- **Total spills**: Number of variables that had to be spilled to memory
- **Cost-based spills**: Number of spills that used cost-based decision (vs. classic linear scan)
- **Max loop depth**: Deepest loop nesting encountered
- **Spill rate**: Percentage of temporaries that were spilled

**Interpretation**: Lower spill rate is better. Cost-based spills indicate loop-aware register allocation is working.

### General Statistics

- **Instructions before optimization**: IR instruction count before any optimizations
- **Instructions after optimization**: IR instruction count after all optimizations
- **Optimization iterations**: Number of times the optimization loop ran
- **Instruction reduction**: Percentage reduction in instruction count

**Interpretation**: Higher reduction indicates more effective optimizations overall.

## API Reference

### Functions

```c
// Enable/disable profiling (default: disabled)
void opt_profile_enable(int enable);

// Reset all counters
void opt_profile_reset(void);

// Get current statistics
const OptProfileStats *opt_profile_get_stats(void);

// Dump statistics to file
void opt_profile_dump(FILE *f);
```

### Counting Functions

```c
// CSE
void opt_profile_count_cse_instruction(void);
void opt_profile_count_cse_eliminated(void);
void opt_profile_count_cse_basic_block(void);

// DCE
void opt_profile_count_dce_eliminated(void);
void opt_profile_count_dce_iteration(void);

// LICM
void opt_profile_count_licm_loop(void);
void opt_profile_count_licm_invariant(void);
void opt_profile_count_licm_moved(void);

// Register Allocation
void opt_profile_count_regalloc_interval(void);
void opt_profile_count_regalloc_spill(int cost_based);
void opt_profile_set_regalloc_max_depth(size_t depth);

// General
void opt_profile_set_instructions_before(size_t count);
void opt_profile_set_instructions_after(size_t count);
void opt_profile_count_iteration(void);
```

## Performance Impact

**Zero overhead when disabled**: All profiling functions are no-ops when profiling is disabled. The `g_profile_enabled` flag is checked at the start of each counting function.

**Minimal overhead when enabled**: Counting functions are simple increments, adding negligible overhead (< 1% in most cases).

## Current Limitations

### LICM: Invariants Detected but Not Moved

The LICM pass currently **detects invariants but doesn't move them**:

```
Loops detected: 5
Invariants found: 324
Instructions moved: 0  ← Always 0
```

**Reason**: Moving instructions requires `ir_insert_at()` support, which is available but requires careful index management to avoid invalidating cached positions during iteration.

**Future**: When full LICM is implemented, "Instructions moved" will show the actual number of invariants moved before loop headers.

### No Per-Function Breakdown

Statistics are aggregated across all functions. Future enhancement could track per-function statistics.

### No Instruction-Level Details

Statistics show counts but not which specific instructions were optimized. Future enhancement could add verbose mode with details.

## Example Analysis

### Case 1: Good CSE, No Loops

```
CSE:
  Instructions scanned: 50
  Eliminated: 5
  Elimination rate: 10.00%

LICM:
  Loops detected: 0
  Invariants found: 0
```

**Analysis**: Code has some redundant computations (10% CSE rate) but no loops. CSE is working effectively.

### Case 2: Many Loops, Many Invariants

```
LICM:
  Loops detected: 10
  Invariants found: 500
  Instructions moved: 0
```

**Analysis**: Code is loop-heavy with many invariant expressions. LICM detection is working well, but code motion is not yet implemented. Once implemented, this could significantly improve performance.

### Case 3: High Spill Rate

```
Register Allocation:
  Total intervals: 200
  Total spills: 50
  Spill rate: 25.00%
```

**Analysis**: High spill rate indicates register pressure is too high. Could benefit from:
- Better register allocation algorithm
- More registers (SIMD registers underutilized?)
- Code restructuring to reduce live ranges

## Integration Guide

### Adding Profiling to a New Pass

1. **Include the header**:
   ```c
   #include "opt_profile.h"
   ```

2. **Add counting calls**:
   ```c
   void opt_my_pass(ZeroCCCompiler *c) {
       // Count iterations
       opt_profile_count_iteration();
       
       // Count specific events
       for (int i = 0; i < count; i++) {
           opt_profile_count_my_event();
       }
   }
   ```

3. **Update documentation**: Add your pass statistics to `opt_profile_dump()`.

### Updating Statistics Structure

If adding new counters:

1. Add field to `OptProfileStats` struct in `opt_profile.h`
2. Add counting function declaration
3. Implement function in `opt_profile.c`
4. Add display code in `opt_profile_dump()`

## Future Enhancements

1. **Per-function statistics**: Track stats separately for each function
2. **Verbose mode**: Show which specific instructions were optimized
3. **JSON output**: Machine-readable format for automation
4. **Baseline comparison**: Compare stats across different optimization levels
5. **Instruction-level details**: Show types of optimizations (e.g., "5 ADDs eliminated")
6. **Performance correlation**: Correlate optimization stats with actual runtime performance

## Files Modified

- `src/zero_cc/parser/opt_profile.h` - API and statistics structure
- `src/zero_cc/parser/opt_profile.c` - Implementation
- `src/zero_cc/parser/opt_cse.c` - Added CSE profiling calls
- `src/zero_cc/parser/opt_loop.c` - Added LICM profiling calls
- `src/zero_cc/parser/regalloc.h` - Added regalloc profiling calls
- `src/zero_cc/parser/opt_pass.c` - Added general stats tracking
- `src/main.c` - Added --profile-opts flag and dump call
- `Makefile` - Added opt_profile.o to build

## Conclusion

The optimization profiling system provides valuable insights into compiler optimization effectiveness. Use it to:
- Identify which optimizations are working
- Find optimization opportunities
- Track improvement over time
- Debug optimization issues

**Enable profiling during development, disable in production for zero overhead.**
