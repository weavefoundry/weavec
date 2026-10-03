// RFC 0030 §5.1, RFC 0032 §13: a variable with a cleanup function calls that function when its
// scope ends, at no call expression. The analysis takes the call for an unknown callee handed
// the variable, so the row may have been released when the second iteration reads it, and the
// read is guarded; the loop is not quiet, so the guard reads the block's state again and traps.
// STAGE: S8
// RUN-INPUT: 2
// ASAN
#include <stdlib.h>
struct row { int cells[4]; };
struct table { struct row *rows; };
static void drop(struct row **p) { free(*p); }
static int total(struct table *t, long n) {
  int sum = 0;
  long j;
  for (j = 0; j < n; j++) {
    struct row *held __attribute__((cleanup(drop))) = t->rows;
    sum += held->cells[0]; // BUG: use-after-free // TRAP: live // GUARDED: temporal
  }
  return sum;
}
int main(int argc, char **argv) {
  struct table t;
  if (argc < 2) return 2;
  t.rows = calloc(1, sizeof *t.rows);
  if (!t.rows) return 2;
  return total(&t, atol(argv[1])) != 0;
}
