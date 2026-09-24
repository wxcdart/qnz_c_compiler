#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

int main(void) {
    const char *src = "int x = 5;\nreturn x;\n";
    char *preprocessed = zero_cc_preprocess(src, NULL);
    if (!preprocessed) {
        fprintf(stderr, "Preprocessing failed\n");
        return 1;
    }
    printf("Preprocessed:\n%s\n", preprocessed);
    free(preprocessed);
    return 0;
}
