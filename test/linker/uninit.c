int g_zero;
char g_buf[64];
static int s_static;
int main(void) {
    s_static = 1;
    g_buf[0] = 65;
    /* g_zero must be zero-initialized without any store */
    return (g_zero == 0 && s_static == 1 && g_buf[0] == 'A') ? 0 : 1;
}
