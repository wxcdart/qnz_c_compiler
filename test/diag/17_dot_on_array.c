struct S { int x; };
int f(void) { struct S d[2] = {{1}, {2}}; return d.x; }
int main(void) { return f(); }
