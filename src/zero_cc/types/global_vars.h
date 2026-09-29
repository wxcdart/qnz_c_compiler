#ifndef ZERO_CC_TYPES_GLOBAL_VARS_H
#define ZERO_CC_TYPES_GLOBAL_VARS_H

/* Global Variable Declaration */
typedef struct {
    char name[128];
    ZeroCCType type;
    size_t data_offset;       /* Offset in data_section */
    size_t size;              /* Size in bytes */
    bool is_static;           /* Internal linkage */
    bool has_initializer;     /* Has = expression */
} ZeroCCGlobalVar;

/* Global variable storage */
#define ZERO_CC_MAX_GLOBALS 256

static ZeroCCGlobalVar global_vars[ZERO_CC_MAX_GLOBALS];
static size_t global_var_count = 0;

/* Reset global variable state at compilation start */
static void reset_global_vars(void) {
    global_var_count = 0;
}

/* Add global variable, return index or -1 on error */
static int add_global_var(const char *name, ZeroCCType type, size_t offset, size_t size, bool is_static, bool has_init) {
    if (global_var_count >= ZERO_CC_MAX_GLOBALS) return -1;
    ZeroCCGlobalVar *gv = &global_vars[global_var_count++];
    strncpy(gv->name, name, sizeof(gv->name) - 1);
    gv->name[sizeof(gv->name) - 1] = '\0';
    gv->type = type;
    gv->data_offset = offset;
    gv->size = size;
    gv->is_static = is_static;
    gv->has_initializer = has_init;
    return global_var_count - 1;
}

/* Find global variable by name, return pointer or NULL */
static ZeroCCGlobalVar *find_global_var(const char *name) {
    for (size_t i = 0; i < global_var_count; i++) {
        if (strcmp(global_vars[i].name, name) == 0) {
            return &global_vars[i];
        }
    }
    return NULL;
}

#endif /* ZERO_CC_TYPES_GLOBAL_VARS_H */
