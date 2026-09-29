#define CONCAT(a, b) a ## b

int main() {
    int CONCAT(var,iable) = 42;  // Should become int variable = 42;
    return variable == 42 ? 0 : 1;
}
