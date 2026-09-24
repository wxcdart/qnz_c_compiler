#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include "zero_cc.h"

int main(void) {
    const char *src = 
        "int foo(void) { return 42; }\n"
        "int main(void) { return foo(); }\n";
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    if (!comp) {
        perror("calloc");
        return 1;
    }
    char *prep = zero_cc_preprocess(src, NULL);
    if (!prep) {
        fprintf(stderr, "Preprocessing failed\n");
        free(comp);
        return 1;
    }
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    zero_cc_compile_c99(comp);
    const char *outfile = "test.exe";
    bool success = zero_cc_emit_elf_exe(comp, outfile);
    if (success) {
        printf("ELF executable emitted to %s\n", outfile);
        pid_t pid = fork();
        if (pid == 0) {
            /* Child */
            execl(outfile, outfile, (char*)NULL);
            _exit(127);
        } else if (pid > 0) {
            int status;
            waitpid(pid, &status, 0);
            if (WIFEXITED(status)) {
                int exit_code = WEXITSTATUS(status);
                printf("Program exited with code %d\n", exit_code);
                free(comp->tokens);
                zero_cc_buf_free(&comp->text_section);
                zero_cc_buf_free(&comp->rodata_section);
                zero_cc_buf_free(&comp->data_section);
                free(comp);
                return (exit_code == 42) ? 0 : 1;
            } else {
                fprintf(stderr, "Child did not exit normally\n");
                free(comp->tokens);
                zero_cc_buf_free(&comp->text_section);
                zero_cc_buf_free(&comp->rodata_section);
                zero_cc_buf_free(&comp->data_section);
                free(comp);
                return 1;
            }
        } else {
            perror("fork");
            free(comp->tokens);
            zero_cc_buf_free(&comp->text_section);
            zero_cc_buf_free(&comp->rodata_section);
            zero_cc_buf_free(&comp->data_section);
            free(comp);
            return 1;
        }
    } else {
        fprintf(stderr, "Failed to emit ELF executable\n");
        free(comp->tokens);
        zero_cc_buf_free(&comp->text_section);
        zero_cc_buf_free(&comp->rodata_section);
        zero_cc_buf_free(&comp->data_section);
        free(comp);
        return 1;
    }
}
