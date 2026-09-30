#include "ir.h"

/* Global instruction buffer (shared across all functions) */
Instr instrs[MAX_INSTRS];
size_t instr_count = 0;
