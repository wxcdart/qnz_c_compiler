/* Pragma state - shared across all translation units */
#include <stdbool.h>

int zero_cc_global_pack = 0;

/* Global diagnostic pragma state */
struct {
    bool warning_as_error;
    char *disabled_warnings[64];
    int disabled_warning_count;
} zero_cc_global_diag_state = {0};
