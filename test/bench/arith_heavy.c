// Benchmark: Arithmetic-heavy code
// Tests constant folding and strength reduction

int main() {
    int x = 100;
    int y = 200;
    int result = 0;
    
    // Constant folding candidates
    result += 10 + 20 + 30;
    result += 100 * 2;
    result += 256 / 4;
    
    // Strength reduction candidates
    for (int i = 0; i < 100; i++) {
        result += i * 2;
        result += i * 4;
        result += i * 8;
        result += i * 16;
    }
    
    return result;
}
