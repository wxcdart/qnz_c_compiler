#include "ir.h"

/* Global instruction buffer (shared across all functions) */
Instr instrs[MAX_INSTRS];
size_t instr_count = 0;

/* Per-call integer argument lists (see ir.h) */
IRCallArgs ir_call_args_table[IR_CALL_MAX_SITES];
size_t ir_call_args_count = 0;
