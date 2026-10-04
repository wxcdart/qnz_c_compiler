/*
 * Task 61 Reentrancy Test
 * Tests that compiler state is properly encapsulated and reentrant
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declarations - these would come from the actual compiler headers */
typedef struct ZeroCCCompiler ZeroCCCompiler;
ZeroCCCompiler *zero_cc_create(void);
void zero_cc_destroy(ZeroCCCompiler *c);
int zero_cc_compile(ZeroCCCompiler *c, const char *source, void **out_code, size_t *out_size);

/* Test: Compile two different functions concurrently (simulated) */
int test_reentrancy(void) {
    const char *source1 = "int func1(int x) { return x + 1; }";
    const char *source2 = "int func2(int x) { return x * 2; }";
    
    ZeroCCCompiler *c1 = zero_cc_create();
    ZeroCCCompiler *c2 = zero_cc_create();
    
    if (!c1 || !c2) {
        fprintf(stderr, "Failed to create compilers\n");
        return 0;
    }
    
    void *code1 = NULL, *code2 = NULL;
    size_t size1 = 0, size2 = 0;
    
    int success = 1;
    
    /* Compile both functions */
    if (!zero_cc_compile(c1, source1, &code1, &size1)) {
        fprintf(stderr, "Failed to compile func1\n");
        success = 0;
    }
    
    if (!zero_cc_compile(c2, source2, &code2, &size2)) {
        fprintf(stderr, "Failed to compile func2\n");
        success = 0;
    }
    
    /* Verify results are different */
    if (success && size1 == size2 && memcmp(code1, code2, size1) == 0) {
        fprintf(stderr, "ERROR: func1 and func2 produced identical code!\n");
        success = 0;
    }
    
    /* Cleanup */
    if (code1) free(code1);
    if (code2) free(code2);
    zero_cc_destroy(c1);
    zero_cc_destroy(c2);
    
    return success;
}

/* Test: Verify no global state leaks between compilations */
int test_no_global_leak(void) {
    const char *source = "int test(int x) { return x + 1; }";
    
    /* Compile multiple times */
    for (int i = 0; i < 10; i++) {
        ZeroCCCompiler *c = zero_cc_create();
        if (!c) return 0;
        
        void *code = NULL;
        size_t size = 0;
        
        if (!zero_cc_compile(c, source, &code, &size)) {
            fprintf(stderr, "Compilation failed on iteration %d\n", i);
            zero_cc_destroy(c);
            return 0;
        }
        
        if (code) free(code);
        zero_cc_destroy(c);
    }
    
    return 1;
}

int main(void) {
    printf("Task 61 Reentrancy Tests\n");
    printf("========================\n");
    
    int passed = 0;
    int total = 0;
    
    total++;
    printf("\nTest 1: Reentrancy (concurrent compilation)... ");
    if (test_reentrancy()) {
        printf("PASS\n");
        passed++;
    } else {
        printf("FAIL\n");
    }
    
    total++;
    printf("Test 2: No global state leak... ");
    if (test_no_global_leak()) {
        printf("PASS\n");
        passed++;
    } else {
        printf("FAIL\n");
    }
    
    printf("\n========================\n");
    printf("Result: %d/%d tests passed\n", passed, total);
    
    return (passed == total) ? 0 : 1;
}
