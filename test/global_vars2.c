int global_x;       /* Uninitialized (zero) */
int global_y = 100; /* Initialized */

int main() {
    int result = global_x + global_y;
    return result == 100 ? 0 : 1;
}
