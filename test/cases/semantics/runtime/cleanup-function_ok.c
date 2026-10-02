// RFC 0030 §5.1, RFC 0032 §13: the correct twin of cleanup-function_bug.c: each iteration has
// its own row, which its cleanup function releases once.
// STAGE: S8
// CLEAN
// RUN-INPUT: 3
// ASAN
#include <stdlib.h>
struct row { int cells[4]; };
static void drop(struct row **p) { free(*p); }
static int total(long n) {
  int sum = 0;
  long j;
  for (j = 0; j < n; j++) {
    struct row *held __attribute__((cleanup(drop))) = calloc(1, sizeof *held);
    if (!held) return -1;
    held->cells[0] = (int)j;
    sum += held->cells[0];
  }
  return sum;
}
int main(int argc, char **argv) {
  if (argc < 2) return 2;
  return total(atol(argv[1])) != 3;
}
