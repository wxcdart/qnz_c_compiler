// AOT Test: Pointers and Memory
// Tests: Pointer arithmetic, dereference, stack variables

int get_value(int* ptr) {
    return *ptr;
}

void set_value(int* ptr, int val) {
    *ptr = val;
}

int* get_max(int* a, int* b) {
    if (*a > *b) {
        return a;
    }
    return b;
}

int main() {
    int x = 10;
    int y = 20;
    
    int val = get_value(&x);
    if (val != 10) return 1;
    
    set_value(&x, 30);
    if (x != 30) return 2;
    
    int* max = get_max(&x, &y);
    if (*max != 30) return 3;
    
    return 0;
}
