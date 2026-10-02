#ifndef ZERO_CC_TYPES_TYPE_SYSTEM_H
#define ZERO_CC_TYPES_TYPE_SYSTEM_H

#include <stdbool.h>

typedef enum {
    TYPE_VOID,
    TYPE_BOOL,
    TYPE_INT8,
    TYPE_UINT8,
    TYPE_INT16,
    TYPE_UINT16,
    TYPE_INT32,
    TYPE_UINT32,
    TYPE_INT64,
    TYPE_UINT64,
    TYPE_FLOAT,
    TYPE_DOUBLE,
    TYPE_NULLPTR,
    TYPE_PTR,
    TYPE_ARRAY,
    TYPE_STRUCT,
    TYPE_UNION,
    /* Reserved range for JIT intrinsics & SIMD vector types */
    TYPE_USER_0,
    TYPE_USER_1,
    TYPE_USER_2,
    TYPE_USER_3
} ZeroTypeTag;

struct ZeroCCType;
typedef struct ZeroCCStructMember {
    char name[128];
    ZeroTypeTag tag;
    size_t size;
    size_t offset;
    size_t align;
    struct ZeroCCType *base;
    bool is_flexible;  /* True for flexible array member (FAM) */
} ZeroCCStructMember;

typedef struct ZeroCCType {
    ZeroTypeTag tag;
    char struct_name[128];
    size_t size;
    size_t align;
    struct ZeroCCType *base;              /* For PTR and ARRAY */
    size_t array_len;                     /* For ARRAY */
    ZeroCCStructMember members[64];       /* For STRUCT */
    bool is_atomic;                       /* For _Atomic */
    bool is_const;
    bool is_volatile;

    size_t member_count;
} ZeroCCType;

/* Type Definitions & Struct Registry */
typedef struct {
    char alias[128];
    ZeroCCType type;
} ZeroCCTypedef;

typedef struct {
    char name[128];
    ZeroCCType type;
} ZeroCCStructDef;

typedef struct {
    char name[128];
    int64_t value;
} ZeroCCEnumConst;

/* Local Symbol Representation */
typedef struct {
    char name[128];
    ZeroCCType type;
    int32_t stack_offset;                 /* Offset from RBP */
    bool is_param;
    int param_reg_idx;
} ZeroCCSym;

/* Function Definition Record */
typedef struct {
    char name[128];
    size_t text_offset;                   /* Byte offset in text_section */
    ZeroCCType return_type;
    size_t param_count;
    char param_names[8][128];
    ZeroCCType param_types[8];
    bool returns_struct;
    size_t ret_struct_sz;
    bool is_variadic;
    bool noreturn;
    bool is_prototype;                    /* True for external prototypes (no body) */
    bool is_external;                     /* True for extern declarations */
} ZeroCCFunction;

/* Relocation Record */
typedef struct {
    size_t text_patch_offset;
    char target_symbol[128];
    bool is_call;
    size_t data_offset;  /* For data references (global variables) */
} ZeroCCReloc;

typedef struct {
    size_t text_patch_offset;
    size_t rodata_offset;
    int dst_reg;  /* Destination register for FP immediate loads */
} ZeroCCStringReloc;


#endif /* ZERO_CC_TYPES_TYPE_SYSTEM_H */
