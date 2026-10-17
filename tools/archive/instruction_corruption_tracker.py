#!/usr/bin/env python3
"""
Instruction Corruption Tracker

Traces where IR instructions get corrupted after ir_emit() is called.
Provides debug instrumentation suggestions to find what overwrites temp_src1 and flags_in.
"""

import sys

def suggest_debug_approach():
    """Suggest the best approach to find the corruption."""
    
    print("""
================================================================================
INSTRUCTION CORRUPTION DEBUG STRATEGY
================================================================================

PROBLEM:
  ir_emit_load() called with correct params (dest=17, offset=-56)
  But instruction stored with temp_src1=255, flags_in=255 (flags register)
  
HYPOTHESIS:
  Something is overwriting the instruction AFTER ir_emit_load() returns
  
APPROACH 1: Binary Search (RECOMMENDED)
----------------------------------------
  1. Add debug print at end of ir_emit_load():
     "ir_emit_load: stored instr[%d] = {dest=%d, temp_src1=%d, flags_in=%d}"
  
  2. Add debug print at start of ir_emit():
     "ir_emit: received instr = {dest=%d, temp_src1=%d, flags_in=%d}"
  
  3. Compare the two - if they differ, corruption happens in ir_emit() itself
  
  4. If same in ir_emit() but different in final instruction array,
     corruption happens AFTER ir_emit() returns

APPROACH 2: Instruction Array Checksum
---------------------------------------
  1. After each ir_emit_*() call, compute checksum of instruction array
  2. If checksum changes unexpectedly, track which instruction changed
  3. Print call stack at that point

APPROACH 3: Memory Protection (Advanced)
-----------------------------------------
  1. Use valgrind to detect memory writes to instruction array
  2. Look for unexpected writes to temp_src1/flags_in fields

QUICK FIX CHECK:
----------------
  Before implementing complex tracking, check these common issues:
  
  1. Is ir_emit_load() returning the correct instruction pointer?
  2. Is the caller using the returned pointer correctly?
  3. Are there any macro expansions that might overwrite the instruction?
  4. Check if temp_src1 and flags_in share memory with other fields (union?)
  
  Look at src/zero_cc/parser/ir_emit_ops.h line ~200-250 for ir_emit_load()

RECOMMENDED FIRST STEP:
-----------------------
  Add these debug prints to src/zero_cc/parser/ir_emit_ops.h:
  
  // In ir_emit_load() function, after storing the instruction:
  fprintf(stderr, "[LOAD] instr[%d] = {opcode=OP_LOAD, dest=%d, offset=%d, ",
          c->curr_instr - 1, dest, offset);
  fprintf(stderr, "temp_src1=%d, flags_in=%d}\\n", 
          c->instructions[c->curr_instr - 1].temp_src1,
          c->instructions[c->curr_instr - 1].flags_in);
  
  This will tell us if corruption happens INSIDE ir_emit_load() or AFTER.

================================================================================
""")

def generate_simple_debug_code():
    """Generate simple debug code snippets to add."""
    
    print("""
================================================================================
DEBUG CODE SNIPPETS TO ADD
================================================================================

SNIPPET 1: Add to src/zero_cc/parser/ir_emit_ops.h in ir_emit_load()
---------------------------------------------------------------------
After the line that stores the instruction (look for c->instructions[c->curr_instr] = load_instr;), add:

    fprintf(stderr, "[LOAD] Just stored: instr[%d] = {opcode=%d, dest=%d, temp_src1=%d, flags_in=%d}\\n",
            c->curr_instr, load_instr.opcode, load_instr.dest, 
            load_instr.temp_src1, load_instr.flags_in);

SNIPPET 2: Add to src/zero_cc/parser/ir.c in ir_emit()
-------------------------------------------------------
At the very start of the function, before any processing:

    fprintf(stderr, "[EMIT] Received: {opcode=%d, dest=%d, temp_src1=%d, flags_in=%d}\\n",
            instr->opcode, instr->dest, instr->temp_src1, instr->flags_in);

SNIPPET 3: Add to src/zero_cc/parser/ir.c after storing instruction
--------------------------------------------------------------------
After the line "c->instructions[c->curr_instr] = *instr;", add:

    fprintf(stderr, "[STORE] After copy: instr[%d] = {opcode=%d, dest=%d, temp_src1=%d, flags_in=%d}\\n",
            c->curr_instr, 
            c->instructions[c->curr_instr].opcode,
            c->instructions[c->curr_instr].dest,
            c->instructions[c->curr_instr].temp_src1,
            c->instructions[c->curr_instr].flags_in);

SNIPPET 4: Add to src/zero_cc/parser/ast_lowering_expr.c after ir_emit_load() call
-----------------------------------------------------------------------------------
After the line that calls ir_emit_load(), add:

    fprintf(stderr, "[CALLER] After ir_emit_load: instr[%d] = {temp_src1=%d, flags_in=%d}\\n",
            c->curr_instr,
            c->instructions[c->curr_instr].temp_src1,
            c->instructions[c->curr_instr].flags_in);

================================================================================
INTERPRETATION GUIDE
================================================================================

If [LOAD] shows CORRECT values but [EMIT] shows WRONG values:
  -> Corruption happens INSIDE ir_emit_load() before calling ir_emit()
  -> Check if load_instr structure is being modified between assignment and call

If [EMIT] shows CORRECT values but [STORE] shows WRONG values:
  -> Corruption happens INSIDE ir_emit() during processing
  -> Check ir_emit() for any modifications to the instr parameter

If [STORE] shows CORRECT values but [CALLER] shows WRONG values:
  -> Corruption happens AFTER ir_emit() returns
  -> Check for subsequent function calls that might overwrite the instruction

If [LOAD] and [CALLER] both show WRONG values:
  -> The instruction structure itself is corrupted before storage
  -> Check if temp_src1/flags_in share memory with other fields (union issue?)

================================================================================
""")

def main():
    if len(sys.argv) < 2:
        suggest_debug_approach()
        print()
        generate_simple_debug_code()
        return
    
    if '--code' in sys.argv:
        generate_simple_debug_code()
    else:
        suggest_debug_approach()

if __name__ == '__main__':
    main()
