int counter = 10;
void bump(void);
int main(void) {
    bump();
    bump();
    return counter == 12 ? 0 : 1;
}
