#include <stdio.h>
const char *greet(void);
int addem(int a, int b);
int main(void) {
    printf("%s %d\n", greet(), addem(20, 22));
    return 0;
}
