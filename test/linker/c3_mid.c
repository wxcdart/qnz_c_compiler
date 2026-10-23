int leaf(int n);
int mid(int n) { return n + leaf(n); }
