/* Test string pointer global */
char *global_str = "hello";

int main() {
    /* Just check that the string pointer is valid */
    return global_str[0] == 'h' && global_str[1] == 'e' ? 0 : 1;
}
