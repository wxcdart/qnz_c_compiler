extern const char *greeting(void);
int main(void) {
    const char *s = greeting();
    if (s[0] != 'h' || s[1] != 'i' || s[2] != '\0') return 1;
    return 0;
}
