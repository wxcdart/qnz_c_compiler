int anon(int, int *);
int main(void) {
    int v = 5;
    return anon(3, &v) == 8 ? 0 : 1;
}
