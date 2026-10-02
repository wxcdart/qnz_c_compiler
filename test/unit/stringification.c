#define STR(x) #x
#define INT_TO_STR(x) STR(x)

int main() {
    const char *s1 = STR(hello);
    const char *s2 = INT_TO_STR(42);
    return s1[0] == 'h' && s2[0] == '4' ? 0 : 1;
}
