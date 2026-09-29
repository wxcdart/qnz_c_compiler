/* Two-Pass IR: Instruction Buffer */
#ifndef ZERO_CC_IR_H
#define ZERO_CC_IR_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>

/* Maximum instructions per function: 64K = 65536 */
#define MAX_INSTRS 65536

/* Instruction structure - 8 bytes packed */
typedef struct __attribute__((packed)) {
    uint8_t  op;        /* Operation code (0-255) */
    uint8_t  temp_dest; /* Destination temp (0-255, 0xFF if none) */
    uint8_t  temp_src1; /* Source temp 1 (0-255, 0xFF if none) */
    uint8_t  temp_src2; /* Source temp 2 (0-255, 0xFF if none) */
    int16_t  imm;       /* Immediate value (-32768 to 32767) */
    uint16_t label_idx; /* Label index for jumps (0-65535) */
} Instr;

/* Global instruction buffer (shared across all functions) */
static Instr instrs[MAX_INSTRS];
static size_t instr_count = 0;

/* Emit instruction - O(1) */
static inline void ir_emit(Instr instr) {
    if (instr_count >= MAX_INSTRS) {
        fprintf(stderr, "Error: Function too large (>64K instructions)\n");
        exit(1);
    }
    instrs[instr_count++] = instr;
}

/* Reset for next function - O(1) */
static inline void ir_reset(void) {
    instr_count = 0;
}

/* Get instruction count */
static inline size_t ir_get_count(void) {
    return instr_count;
}

/* Access instruction by index */
static inline Instr* ir_get(size_t idx) {
    if (idx >= instr_count) return NULL;
    return &instrs[idx];
}

#endif /* ZERO_CC_IR_H */
