#define CONCAT(a, b) a ## b

int var1 = 10;
int var2 = 20;

int main() {
    int x = CONCAT(var, 1) + CONCAT(var, 2);  // var1 + var2 = 30
    return x == 30 ? 0 : 1;
}
