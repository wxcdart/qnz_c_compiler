int main() {
    int x = 0;
loop:
    x++;
    if (x < 3) goto loop;
    return x == 3 ? 0 : 1;
}
