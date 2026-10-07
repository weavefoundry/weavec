// RFC 0031 §4.1, §4.7 I1, I4: a weak update keeps both the old and the new value of each cell it may reach.
// STAGE: S2
// 'p' points to 'a' or to 'b', and a pointer to the live local 'z' is stored through it.
// Each of 'a' and 'b' holds either its old target or 'z', all of them live, non-null ints,
// so both dereferences are proven (no null facet is checked or left unresolved).
// RUN-INPUT:
// RUN-INPUT: 1
// CLEAN
// ASAN
int main(int argc, char **argv) {
  (void)argv;
  int x = 1, y = 2, z = 3;
  int *a = &x, *b = &y;
  int **p = argc > 1 ? &a : &b;
  *p = &z;
  int r = *a + *b;
  return r == 4 || r == 5 ? 0 : 1;
}
