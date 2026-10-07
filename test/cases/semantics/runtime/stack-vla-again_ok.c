// RFC 0032 §13: the correct twin of stack-vla-again_bug.c: the last byte of the second array.
// STAGE: S8
// CLEAN
// RUN-INPUT: 7
// ASAN
#include <stdlib.h>
#include <string.h>
static char *same(char *p) { return p; }
static char *(*volatile hide)(char *) = same;
static long sum(long first, long second, long idx) {
  long total = 0, i;
  int round = 0;
again:;
  char v[round ? second : first];
  char *p = hide(v);
  memset(v, 1, sizeof v);
  for (i = 0; i < 3; i++)
    total += p[round ? idx : 0];
  if (round++ == 0)
    goto again;
  return total;
}
int main(int argc, char **argv) {
  if (argc < 2) return 2;
  return sum(64, 8, atol(argv[1])) == 0;
}
