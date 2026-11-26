#include "zero_cc/opt/opt_pgo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zero_cc/ir/ir.h"
#include "zero_cc/ir/ir_emitter.h"
#include "zero_cc/types/symbols.h"
#include "zero_cc/types/vregs.h"

ZeroCCPGOProfile *zc_pgo_load_profile(const char *path) {
    if (!path) return NULL;
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    ZeroCCPGOProfile *prof = (ZeroCCPGOProfile *)calloc(1, sizeof(ZeroCCPGOProfile));
    if (!prof) {
        fclose(f);
        return NULL;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char fn[96];
        unsigned block_id = 0;
        unsigned long long count = 0;
        if (sscanf(line, "%95s %u %llu", fn, &block_id, &count) == 3) {
            if (prof->count >= prof->capacity) {
                size_t new_cap = prof->capacity ? prof->capacity * 2 : 64;
                ZeroCCPGORecord *new_recs = (ZeroCCPGORecord *)realloc(prof->records, new_cap * sizeof(ZeroCCPGORecord));
                if (!new_recs) break;
                prof->records = new_recs;
                prof->capacity = new_cap;
            }
            prof->records[prof->count].fn_name = strdup(fn);
            prof->records[prof->count].block_id = block_id;
            prof->records[prof->count].count = count;
            prof->total_hits += count;
            prof->count++;
        }
    }
    fclose(f);
    return prof;
}

void zc_pgo_free_profile(ZeroCCPGOProfile *prof) {
    if (!prof) return;
    for (size_t i = 0; i < prof->count; i++) {
        free(prof->records[i].fn_name);
    }
    free(prof->records);
    free(prof);
}

uint64_t zc_pgo_get_block_count(const ZeroCCPGOProfile *prof, const char *fn_name, uint32_t block_id) {
    if (!prof || !fn_name) return 0;
    for (size_t i = 0; i < prof->count; i++) {
        if (prof->records[i].block_id == block_id &&
            strcmp(prof->records[i].fn_name, fn_name) == 0) {
            return prof->records[i].count;
        }
    }
    return 0;
}

uint64_t zc_pgo_get_fn_count(const ZeroCCPGOProfile *prof, const char *fn_name) {
    /* Block 0 is the function entry */
    return zc_pgo_get_block_count(prof, fn_name, 0);
}

bool zc_pgo_is_fn_hot(const ZeroCCPGOProfile *prof, const char *fn_name) {
    uint64_t c = zc_pgo_get_fn_count(prof, fn_name);
    return (c >= 10);
}

bool zc_pgo_is_fn_cold(const ZeroCCPGOProfile *prof, const char *fn_name) {
    if (!prof) return false;
    uint64_t c = zc_pgo_get_fn_count(prof, fn_name);
    return (c == 0);
}

void zc_pgo_emit_hit(ZeroCCCompiler *c, const char *fn_name, uint32_t block_id) {
    if (!c || !c->profile_generate || !fn_name) return;
    if (c->pgo_fn_name_rodata_offset == (size_t)-1) {
        c->pgo_fn_name_rodata_offset = c->rodata_section.length;
        zero_cc_buf_put_bytes(&c->rodata_section, fn_name, strlen(fn_name) + 1);
    }
    int t_name = zc_alloc_temp(c);
    ir_emit_lea_rip(c, t_name, c->pgo_fn_name_rodata_offset);
    int t_block = zc_alloc_temp(c);
    ir_emit_mov_imm(c, t_block, (int64_t)block_id);
    ir_emit_call(c, "__zc_pgo_hit");
    if (ir_get_count(c) > 0) {
        Instr *last = (Instr*)&ir_get_buffer(c)[ir_get_count(c) - 1];
        last->temp_dest = 2;
        last->temp_src1 = (uint32_t)t_name;
        last->temp_src2 = (uint32_t)t_block;
        last->imm = 0x0000FFFF; /* stack_pos 255 for arg 0 and arg 1 = register args */
        last->cond = 0;
        last->width = 0;
    }
}
