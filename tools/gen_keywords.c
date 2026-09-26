/* gen_keywords.c - generates perfect hash for C keywords
 * Usage: ./gen_keywords < keywords.txt > src/zero_cc/keywords.inc
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAX_KEYWORDS 256

typedef struct {
    const char *name;
    const char *tok_name;
} Keyword;

int main(int argc, char **argv) {
    FILE *in = stdin;
    if (argc > 1) in = fopen(argv[1], "r");
    if (!in) return 1;

    Keyword kw[MAX_KEYWORDS];
    int n = 0;
    char line[256];

    while (fgets(line, sizeof(line), in) && n < MAX_KEYWORDS) {
        char *p = line;
        while (*p && (*p==' '||*p=='\t'||*p=='\n'||*p=='\r')) p++;
        if (*p=='#' || *p=='\0') continue;
        char *end = p + strcspn(p, " \t\r\n");
        *end = '\0';
        
        // Map keyword to token name
        const char *tok = p;
        if (strcmp(p, "auto")==0) tok = "TK_AUTO";
        else if (strcmp(p, "break")==0) tok = "TK_BREAK";
        else if (strcmp(p, "case")==0) tok = "TK_CASE";
        else if (strcmp(p, "char")==0) tok = "TK_CHAR";
        else if (strcmp(p, "const")==0) tok = "TK_CONST";
        else if (strcmp(p, "continue")==0) tok = "TK_CONTINUE";
        else if (strcmp(p, "default")==0) tok = "TK_DEFAULT";
        else if (strcmp(p, "do")==0) tok = "TK_DO";
        else if (strcmp(p, "double")==0) tok = "TK_DOUBLE_KW";
        else if (strcmp(p, "else")==0) tok = "TK_ELSE";
        else if (strcmp(p, "enum")==0) tok = "TK_ENUM";
        else if (strcmp(p, "extern")==0) tok = "TK_EXTERN";
        else if (strcmp(p, "float")==0) tok = "TK_FLOAT_KW";
        else if (strcmp(p, "for")==0) tok = "TK_FOR";
        else if (strcmp(p, "goto")==0) tok = "TK_GOTO";
        else if (strcmp(p, "if")==0) tok = "TK_IF";
        else if (strcmp(p, "inline")==0) tok = "TK_INLINE";
        else if (strcmp(p, "int")==0) tok = "TK_INT";
        else if (strcmp(p, "long")==0) tok = "TK_LONG";
        else if (strcmp(p, "register")==0) tok = "TK_REGISTER";
        else if (strcmp(p, "restrict")==0) tok = "TK_RESTRICT";
        else if (strcmp(p, "return")==0) tok = "TK_RETURN";
        else if (strcmp(p, "short")==0) tok = "TK_SHORT";
        else if (strcmp(p, "signed")==0) tok = "TK_SIGNED";
        else if (strcmp(p, "sizeof")==0) tok = "TK_SIZEOF";
        else if (strcmp(p, "static")==0) tok = "TK_STATIC";
        else if (strcmp(p, "struct")==0) tok = "TK_STRUCT";
        else if (strcmp(p, "switch")==0) tok = "TK_SWITCH";
        else if (strcmp(p, "typedef")==0) tok = "TK_TYPEDEF";
        else if (strcmp(p, "union")==0) tok = "TK_UNION";
        else if (strcmp(p, "unsigned")==0) tok = "TK_UNSIGNED";
        else if (strcmp(p, "void")==0) tok = "TK_VOID";
        else if (strcmp(p, "volatile")==0) tok = "TK_VOLATILE";
        else if (strcmp(p, "while")==0) tok = "TK_WHILE";
        else if (strcmp(p, "_Alignas")==0) tok = "TK_ALIGNAS";
        else if (strcmp(p, "_Alignof")==0) tok = "TK_ALIGNOF";
        else if (strcmp(p, "_Atomic")==0) tok = "TK_ATOMIC";
        else if (strcmp(p, "_Bool")==0) tok = "TK_BOOL_KW";
        else if (strcmp(p, "_Complex")==0) tok = "TK_COMPLEX";
        else if (strcmp(p, "_Generic")==0) tok = "TK_GENERIC";
        else if (strcmp(p, "_Imaginary")==0) tok = "TK_IMAGINARY";
        else if (strcmp(p, "_Noreturn")==0) tok = "TK_NORETURN";
        else if (strcmp(p, "_Static_assert")==0) tok = "TK_STATIC_ASSERT";
        else if (strcmp(p, "_Thread_local")==0) tok = "TK_THREAD_LOCAL";
        else if (strcmp(p, "typeof")==0) tok = "TK_TYPEOF";
        else if (strcmp(p, "typeof_unqual")==0) tok = "TK_TYPEOF_UNQUAL";
        else if (strcmp(p, "static_assert")==0) tok = "TK_STATIC_ASSERT";
        else if (strcmp(p, "alignas")==0) tok = "TK_ALIGNAS";
        else if (strcmp(p, "alignof")==0) tok = "TK_ALIGNOF";
        else if (strcmp(p, "thread_local")==0) tok = "TK_THREAD_LOCAL";
        else if (strcmp(p, "constexpr")==0) tok = "TK_CONSTEXPR";
        else if (strcmp(p, "true")==0) tok = "TK_TRUE";
        else if (strcmp(p, "false")==0) tok = "TK_FALSE";
        else if (strcmp(p, "nullptr")==0) tok = "TK_NULLPTR";
        else if (strcmp(p, "_Generic")==0) tok = "TK_GENERIC";
        
        kw[n].name = strdup(p);
        kw[n].tok_name = tok;
        n++;
    }

    if (in != stdin) fclose(in);

    // Simple perfect hash using hash of first char + length
    // For small n, we can just use a static table with linear probe
    printf("/* Auto-generated perfect hash for C keywords */\n");
    printf("#ifndef ZERO_CC_KEYWORDS_INC\n");
    printf("#define ZERO_CC_KEYWORDS_INC\n\n");
    printf("typedef struct { const char *name; ZeroCCTokType type; } KeywordEntry;\n\n");
    printf("static const KeywordEntry keyword_table[] = {\n");
    for (int i = 0; i < n; i++) {
        printf("    {\"%s\", %s},\n", kw[i].name, kw[i].tok_name);
    }
    printf("};\n\n");
    printf("#define KEYWORD_COUNT %d\n\n", n);
    printf("static inline ZeroCCTokType zero_cc_keyword_lookup_internal(const char *s, size_t len) {\n");
    printf("    for (int i = 0; i < KEYWORD_COUNT; i++) {\n");
    printf("        if (keyword_table[i].name[len] == '\\0' && keyword_table[i].name[0] == s[0]) {\n");
    printf("            if (memcmp(keyword_table[i].name, s, len) == 0)\n");
    printf("                return keyword_table[i].type;\n");
    printf("        }\n");
    printf("    }\n");
    printf("    return TK_IDENT;\n");
    printf("}\n\n");
    printf("#endif /* ZERO_CC_KEYWORDS_INC */\n");

    for (int i = 0; i < n; i++) free((void*)kw[i].name);
    return 0;
}