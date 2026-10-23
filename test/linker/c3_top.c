int mid(int n);
int main(void) {
    /* mid(n) = n + leaf(n); leaf(n) = n * 2  ->  main checks n*3+... */
    return mid(5) == 15 ? 0 : 1;
}
