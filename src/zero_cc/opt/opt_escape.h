#ifndef ZERO_CC_OPT_ESCAPE_H
#define ZERO_CC_OPT_ESCAPE_H

#include <stdbool.h>

typedef struct ZeroCCCompiler ZeroCCCompiler;

/*
 * Escape Analysis & Heap-to-Stack Promotion - Sprint 233
 *
 * Performs intra-procedural escape analysis on dynamically allocated heap memory:
 * 1. Identifies calls to malloc(size) with compile-time bounded size (size <= 4096).
 * 2. Tracks the returned pointer, copies, stack-saved slots, and derived pointers.
 * 3. Verifies that the pointer does not escape the current function:
 *    - Not returned or assigned to return register (VREG_0).
 *    - Not stored into global memory, struct fields, or other pointer targets.
 *    - Not passed as an argument to non-free function calls.
 *    - Not used in indirect calls, asm, or setjmp.
 * 4. Heap-to-Stack Promotion:
 *    - Replaces malloc(size) with OP_LEA dest, [rbp - stack_slot], allocating
 *      a dedicated stack slot in the current frame.
 *    - Replaces corresponding free(ptr) calls with OP_NOP.
 * 5. Dead Allocation Elimination:
 *    - If the allocated pointer is never read or written (only malloc + free),
 *      eliminates both operations entirely with zero stack allocation.
 */
bool opt_escape(ZeroCCCompiler *c);

#endif /* ZERO_CC_OPT_ESCAPE_H */
