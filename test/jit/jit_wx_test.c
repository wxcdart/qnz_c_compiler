#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#if defined(__unix__) || defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

static void segv_handler(int sig) {
    (void)sig;
    _exit(42); /* Caught expected fault from writing to RX page */
}
#endif

#include "zero_cc.h"

static int test_mmap_wx_invariants(void) {
    size_t page_sz = zero_get_page_size();
    if (page_sz < 4096) {
        fprintf(stderr, "Page size %zu is smaller than expected minimum 4096\n", page_sz);
        return 1;
    }

    /* 1. Allocate RW page */
    void *mem = zero_mmap_alloc_rw(page_sz);
    if (!mem) {
        fprintf(stderr, "zero_mmap_alloc_rw failed\n");
        return 1;
    }

    /* 2. Write to RW page */
    volatile unsigned char *p = (volatile unsigned char *)mem;
    p[0] = 0x90; /* NOP */
    p[1] = 0xC3; /* RET */
    if (p[0] != 0x90 || p[1] != 0xC3) {
        fprintf(stderr, "Read-after-write failed on RW page\n");
        zero_mmap_free(mem, page_sz);
        return 1;
    }

    /* 3. Transition to RX (strict W^X: write is disabled) */
    if (zero_mmap_make_rx(mem, page_sz) != 0) {
        fprintf(stderr, "zero_mmap_make_rx failed\n");
        zero_mmap_free(mem, page_sz);
        return 1;
    }

    /* 4. Execution check: call the function */
    int (*fn)(void) = (int (*)(void))mem;
    fn();

#if defined(__unix__) || defined(__linux__) || defined(__APPLE__)
    /* 5. Verify Write-protection in child process */
    pid_t pid = fork();
    if (pid == 0) {
        /* Set handler so child exits cleanly with code 42 on fault */
        signal(SIGSEGV, segv_handler);
        signal(SIGBUS, segv_handler);
        /* Attempt to write to RX page. This MUST trigger SIGSEGV or SIGBUS. */
        p[0] = 0xCC;
        /* If we reached here, write succeeded without signal, which is a W^X violation */
        _exit(0);
    } else if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            if (code == 0) {
                fprintf(stderr, "W^X violation: write to RX page succeeded!\n");
                zero_mmap_free(mem, page_sz);
                return 1;
            }
            if (code != 42) {
                fprintf(stderr, "Unexpected child exit code: %d\n", code);
                zero_mmap_free(mem, page_sz);
                return 1;
            }
        } else if (!WIFSIGNALED(status) || (WTERMSIG(status) != SIGSEGV && WTERMSIG(status) != SIGBUS)) {
            fprintf(stderr, "Unexpected child termination status: %d\n", status);
            zero_mmap_free(mem, page_sz);
            return 1;
        }
    } else {
        perror("fork");
        zero_mmap_free(mem, page_sz);
        return 1;
    }
#endif

    /* 6. Clean up */
    zero_mmap_free(mem, page_sz);
    return 0;
}

static int test_jit_wx_execution(void) {
    const char *src =
        "int compute(int a, int b) {\n"
        "    return a * b + 2;\n"
        "}\n"
        "int main(void) {\n"
        "    return compute(5, 8);\n"
        "}\n";

    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    if (!comp) return 1;

    char *prep = zero_cc_preprocess(src, NULL);
    if (!prep) {
        free(comp);
        return 1;
    }

    zero_cc_tokenize_c99(prep, comp);
    free(prep);

    zero_cc_compile_c99(comp);

    int result = zero_cc_execute_jit(comp, NULL);

    zero_cc_compiler_free(comp);

    if (result != 42) {
        fprintf(stderr, "JIT execution failed: expected 42, got %d\n", result);
        return 1;
    }
    return 0;
}

int main(void) {
    if (test_mmap_wx_invariants() != 0) {
        fprintf(stderr, "test_mmap_wx_invariants FAILED\n");
        return 1;
    }
    if (test_jit_wx_execution() != 0) {
        fprintf(stderr, "test_jit_wx_execution FAILED\n");
        return 1;
    }
    printf("JIT W^X memory hardening tests PASSED\n");
    return 0;
}
