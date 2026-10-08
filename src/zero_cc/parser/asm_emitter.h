/* Assembly Emitter - IR to Assembly Translation
 *
 * This module provides ISA-agnostic assembly emission from IR.
 * It translates IR instructions to GNU assembler syntax using the
 * backend's register information for ISA-specific details.
 *
 * Usage:
 *   emit_ir_to_asm(c, spill_base_offset);
 *
 * The emitter uses BE(c)->regs for register names and BE(c)->conv
 * for calling convention details.
 */

#ifndef ZERO_CC_PARSER_ASM_EMITTER_H
#define ZERO_CC_PARSER_ASM_EMITTER_H

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include "ir.h"
#include "../types/symbols.h"

/*
 * Emit assembly from IR with allocated registers.
 *
 * This is the main entry point for assembly emission. It iterates through
 * the IR instruction buffer and emits GNU assembler syntax for each instruction.
 *
 * Parameters:
 *   c              - Compiler context (contains IR, register allocator, backend info)
 *   spill_base_offset - Base offset for spilled temporaries (may be unused)
 *
 * Output:
 *   Assembly is written to c->asm_out if set, otherwise to stdout.
 */
void emit_ir_to_asm(ZeroCCCompiler *c, int32_t spill_base_offset);

/*
 * Emit a single IR instruction as assembly.
 *
 * Internal function used by emit_ir_to_asm. Exposed for testing.
 *
 * Parameters:
 *   c     - Compiler context
 *   instr - IR instruction to emit
 *   idx   - Instruction index (for debugging comments)
 */
void asm_emit_instruction(ZeroCCCompiler *c, const Instr *instr, size_t idx, size_t func_idx);

/*
 * Emit function prologue as assembly.
 *
 * Parameters:
 *   out        - Output file
 *   frame_size - Total stack frame size
 */
void asm_emit_prologue(FILE *out, size_t frame_size);

/*
 * Emit function epilogue as assembly.
 *
 * Parameters:
 *   out - Output file
 */
void asm_emit_epilogue(FILE *out);

/*
 * Emit a label as assembly.
 *
 * Parameters:
 *   out     - Output file
 *   label   - Label name
 */
void asm_emit_label(FILE *out, const char *label);

/*
 * Get register name for a physical register number.
 *
 * Parameters:
 *   reg - Physical register number
 *   is_fp - True for floating-point registers
 *
 * Returns:
 *   Register name string (e.g., "%rax", "%xmm0")
 */
const char *asm_get_reg_name(int reg, int is_fp);

#endif /* ZERO_CC_PARSER_ASM_EMITTER_H */
