int main() {
    int x = 0;
loop:
    x++;
    if (x < 3) goto loop;
    return x;
}
