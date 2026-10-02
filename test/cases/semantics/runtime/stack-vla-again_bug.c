// RFC 0032 §13, amendment 21: a variable-length array can be declared again with another size
// when a goto runs its declaration a second time, so a range cache does not keep it: the second
// round's 8 bytes are checked, not the first round's 64.
// STAGE: S8
// RUN-INPUT: 8
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
    total += p[round ? idx : 0]; // BUG: out-of-bounds // TRAP: object // GUARDED: spatial
  if (round++ == 0)
    goto again;
  return total;
}
int main(int argc, char **argv) {
  if (argc < 2) return 2;
  return sum(64, 8, atol(argv[1])) == 0;
}
