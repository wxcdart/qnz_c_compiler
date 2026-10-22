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
            unlink(outfile);
            if (WIFEXITED(status)) {
                int exit_code = WEXITSTATUS(status);
                printf("Program exited with code %d\n", exit_code);
                zero_cc_compiler_free(comp);
                return (exit_code == 42) ? 0 : 1;
            } else {
                fprintf(stderr, "Child did not exit normally\n");
                zero_cc_compiler_free(comp);
                return 1;
            }
        } else {
            perror("fork");
            unlink(outfile);
            zero_cc_compiler_free(comp);
            return 1;
        }
    } else {
        fprintf(stderr, "Failed to emit ELF executable\n");
        zero_cc_compiler_free(comp);
        return 1;
    }
}
