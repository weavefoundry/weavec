// RFC 0030 §7.2 (WEAVEC_COUNTED_BY on a parameter): a declared count is checked at every call.
// STAGE: S6
// Counted(n) is authoritative for callers: the call is checked with the len template before
// the call (BeforeCall, §10.4). Inside 'sum' the declaration holds under A1, so p[i] for
// i < n is proven. The run passes n == 5 for an int[4]; ASan reports the read in 'sum',
// whose enclosing call is the checked row.
// RUN-INPUT: 5
// ASAN
#include <stdlib.h>
#include "../Inputs/rfc0030.h"

int sum(const int *WEAVEC_COUNTED_BY(n) p, size_t n) {
  int s = 0;
  for (size_t i = 0; i < n; i++) s += p[i];
  return s;
}

int main(int argc, char **argv) {
  int a[4] = {1, 2, 3, 4};
  size_t n = argc > 1 ? (size_t)atoi(argv[1]) : 4;
  return sum(a, n); // BUG: out-of-bounds // TRAP: len
}
