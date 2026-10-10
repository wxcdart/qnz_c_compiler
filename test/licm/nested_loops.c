/* LICM Test: nested_loops */
int test(int **mat, int rows, int cols, int factor) {
    int result = 0;
    for (int i = 0; i < rows; i++) {
        int row_offset = i * cols;
        for (int j = 0; j < cols; j++) {
            result += mat[i][j] * factor;
        }
    }
    return result;
}

int main() {
    int row1[3] = {1, 2, 3};
    int row2[3] = {4, 5, 6};
    int *mat[2] = {row1, row2};
    return test(mat, 2, 3, 2);  /* Expected: (1+2+3)*2 + (4+5+6)*2 = 12 + 30 = 42 */
}
