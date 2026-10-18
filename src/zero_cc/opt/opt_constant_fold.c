#include "opt_constant_fold.h"
#include "../ir/ir.h"
#include "../ir/ir_emitter.h"
#include <stdlib.h>
#include <string.h>

/**
 * Constant value storage
 * Maps temp index to constant value
 */
#define CONST_MAP_SIZE 4096
static int g_const_values[CONST_MAP_SIZE];
static bool g_is_const[CONST_MAP_SIZE];

/**
 * Check if a temp holds a constant value
 */
int ir_is_constant(int temp) {
    if (temp < 0 || temp >= CONST_MAP_SIZE) {
        return 0;
    }
    return g_is_const[temp] ? 1 : 0;
}

/**
 * Get constant value from temp
 */
int ir_get_constant(int temp) {
    if (temp < 0 || temp >= CONST_MAP_SIZE) {
        return 0;
    }
    return g_const_values[temp];
}

/**
 * Mark a temp as holding a constant
 */
static void mark_constant(int temp, int value) {
    if (temp >= 0 && temp < CONST_MAP_SIZE) {
        g_is_const[temp] = true;
        g_const_values[temp] = value;
    }
}

/**
 * Clear constant tracking
 */
static void clear_constants(void) {
    memset(g_is_const, 0, sizeof(g_is_const));
    memset(g_const_values, 0, sizeof(g_const_values));
}

/**
 * Fold binary operation with two constants
 */
static int fold_binop(int op, int left, int right) {
    switch (op) {
        case OP_ADD: return left + right;
        case OP_SUB: return left - right;
        case OP_MUL: return left * right;
        case OP_DIV: return (right != 0) ? left / right : 0;
        case OP_MOD: return (right != 0) ? left % right : 0;
        case OP_AND: return left & right;
        case OP_OR:  return left | right;
        case OP_XOR: return left ^ right;
        case OP_SHL: return left << right;
        case OP_SHR: return left >> right;
        case OP_SAR: return left >> right;
        default: return 0;
    }
}

/**
 * Constant folding pass
 * 
 * Strategy: Only fold binary operations where BOTH operands are constants.
 * We replace the entire instruction with OP_MOV_IMM to the result.
 * 
 * IMPORTANT: This pass runs BEFORE register allocation, so we can safely
 * modify the IR structure without breaking live range analysis.
 */
void opt_fold_constants(ZeroCCCompiler *c) {
    (void)c;
    
    clear_constants();
    
    int count = (int)ir_get_count();
    
    // First pass: collect all constant definitions from OP_MOV_IMM
    for (int i = 0; i < count; i++) {
        Instr *instr = ir_get(i);
        if (!instr) continue;
        
        int dest = instr->temp_dest;
        if (dest == 0xFF || dest == 0) continue;
        
        // Track constants from OP_MOV_IMM
        if (instr->op == OP_MOV_IMM) {
            // Use imm64 for full 64-bit precision
            mark_constant(dest, (int)instr->imm64);
        }
    }
    
    // Second pass: fold binary operations with two constant operands
    for (int i = 0; i < count; i++) {
        Instr *instr = ir_get(i);
        if (!instr) continue;
        
        // Fold binary operations with constants
        if (instr->op == OP_ADD || instr->op == OP_SUB || 
            instr->op == OP_MUL || instr->op == OP_DIV ||
            instr->op == OP_MOD || instr->op == OP_AND ||
            instr->op == OP_OR || instr->op == OP_XOR ||
            instr->op == OP_SHL || instr->op == OP_SHR ||
            instr->op == OP_SAR) {
            
            int left_const = ir_is_constant(instr->temp_src1);
            int right_const = ir_is_constant(instr->temp_src2);
            
            if (left_const && right_const) {
                // Both operands are constants - fold them
                int left_val = ir_get_constant(instr->temp_src1);
                int right_val = ir_get_constant(instr->temp_src2);
                int result = fold_binop(instr->op, left_val, right_val);
                
                // Replace with MOV_IMM - preserve dest, set src to 0xFF
                int dest = instr->temp_dest;
                instr->op = OP_MOV_IMM;
                instr->temp_dest = dest;
                instr->imm = (int16_t)result;      // 16-bit for compatibility
                instr->imm64 = result;              // 64-bit for emitter
                instr->temp_src1= TEMP_NONE;
                instr->temp_src2= TEMP_NONE;
                instr->label_idx = 0;
                
                // Mark the result as constant for future folding
                mark_constant(dest, result);
            }
        }
    }
}
