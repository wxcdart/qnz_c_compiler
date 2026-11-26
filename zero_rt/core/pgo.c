#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PGO_COUNTERS 8192

typedef struct {
    char fn_name[96];
    uint32_t block_id;
    uint64_t count;
} PGOCounter;

static PGOCounter s_pgo_counters[MAX_PGO_COUNTERS];
static size_t s_pgo_count = 0;
static char s_pgo_output_path[512] = "default.zcprof";
static int s_pgo_dumped = 0;
static int s_pgo_registered = 0;

void __zc_pgo_set_file(const char *path) {
    if (path && *path) {
        strncpy(s_pgo_output_path, path, sizeof(s_pgo_output_path) - 1);
        s_pgo_output_path[sizeof(s_pgo_output_path) - 1] = '\0';
    }
}

void __zc_pgo_dump(void) {
    if (s_pgo_dumped) return;
    s_pgo_dumped = 1;
    const char *env = getenv("ZERO_CC_PROFILE_FILE");
    const char *out_path = (env && *env) ? env : s_pgo_output_path;
    FILE *f = fopen(out_path, "w");
    if (!f) return;
    fprintf(f, "# ZERO_CC_PGO_PROFILE_V1\n");
    for (size_t i = 0; i < s_pgo_count; i++) {
        fprintf(f, "%s %u %llu\n", s_pgo_counters[i].fn_name,
                (unsigned)s_pgo_counters[i].block_id,
                (unsigned long long)s_pgo_counters[i].count);
    }
    fclose(f);
}

void __zc_pgo_hit(const char *fn_name, uint32_t block_id) {
    if (!fn_name) return;
    if (!s_pgo_registered) {
        s_pgo_registered = 1;
        atexit(__zc_pgo_dump);
    }
    for (size_t i = 0; i < s_pgo_count; i++) {
        if (s_pgo_counters[i].block_id == block_id &&
            strcmp(s_pgo_counters[i].fn_name, fn_name) == 0) {
            s_pgo_counters[i].count++;
            return;
        }
    }
    if (s_pgo_count < MAX_PGO_COUNTERS) {
        size_t idx = s_pgo_count++;
        strncpy(s_pgo_counters[idx].fn_name, fn_name, sizeof(s_pgo_counters[idx].fn_name) - 1);
        s_pgo_counters[idx].fn_name[sizeof(s_pgo_counters[idx].fn_name) - 1] = '\0';
        s_pgo_counters[idx].block_id = block_id;
        s_pgo_counters[idx].count = 1;
    }
}

void __zc_pgo_reset(void) {
    s_pgo_count = 0;
    s_pgo_dumped = 0;
    s_pgo_registered = 0;
}
