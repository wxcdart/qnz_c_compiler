#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    FILE *in = stdin;
    if (argc > 1) in = fopen(argv[1], "r");
    if (!in) return 1;

    char line[256];
    printf("/* Auto-generated keywords table */\n");
    printf("static const struct { const char *name; int type; } kw_table[] = {\n");
    int idx = 1000;
    while (fgets(line, sizeof(line), in)) {
        char *p = line;
        while (*p && (*p==' '||*p=='\t'||*p=='\n'||*p=='\r')) p++;
        if (*p=='#' || *p=='\0') continue;
        char *end = p + strcspn(p, " \t\r\n");
        *end = '\0';
        printf("  {\"%s\", %d},\n", p, idx++);
    }
    printf("};\n");
    if (in != stdin) fclose(in);
    return 0;
}
