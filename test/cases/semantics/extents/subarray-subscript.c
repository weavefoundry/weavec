// RFC 0030 §7.4: a direct subscript of a non-flexible array-typed lvalue uses the array's own bound.
// STAGE: S3
// For int m[3][4], m[i][j] needs j < 4, as C does, even though m[0][4] lies inside m. The
// index is unknown at compile time, so the access is checked with the index template; the
// run passes j == 4. ASan's array-bounds check confirms the defect.
// RUN-INPUT: 4
// ASAN
#include <stdlib.h>

int get(int i, int j) {
  int m[3][4] = {{0}};
  return m[i][j]; // BUG: out-of-bounds // TRAP: index
}

int main(int argc, char **argv) {
  return get(0, argc > 1 ? atoi(argv[1]) : 0);
}
