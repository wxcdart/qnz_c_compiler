/* Variadic Test: printf-style */
#include <stdarg.h>
#include <assert.h>

int my_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int count = 0;
    while (*fmt) {
        if (*fmt == '%' && *(fmt+1) == 'd') {
            int val = va_arg(ap, int);
            count += val;
            fmt += 2;
        } else {
            fmt++;
        }
    }
    va_end(ap);
    return count;
}

int sum_varargs(int count, ...) {
    va_list ap;
    va_start(ap, count);
    int sum = 0;
    for (int i = 0; i < count; i++) {
        sum += va_arg(ap, int);
    }
    va_end(ap);
    return sum;
}

int main(void) {
    /* Test va_arg with known count */
    assert(sum_varargs(5, 1,2,3,4,5) == 15);
    assert(sum_varargs(3, 10,20,30) == 60);
    
    /* Test format string parsing (simplified) */
    assert(my_printf("%d %d %d", 1, 2, 3) == 6);
    
    return 0;
}