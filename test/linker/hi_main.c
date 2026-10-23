int add(int a, int b);
int main(void) {
    int r = add(3, 4);
    r = r * 2 + add(-1, 1);
    return r == 14 ? 0 : 1;
}
