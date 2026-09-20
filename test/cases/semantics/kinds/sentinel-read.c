// RFC 0030 §7.1: a sentinel read p[n] after a loop over p[0..n) is not checked against 'n'.
// STAGE: S6
// The loop gives 0 < n -> Counted(n) (R2) and the unguarded p[n] after it gives
// Counted(n + 1) (R5). 'last_is_end' is exported, so the accesses these inferred
// requirements cover are trusted(caller-contract) inside the body (§7.5). An inferred
// requirement is a lower bound, and a check of p[n] against the loop's 'n' would trap every
// correct sentinel read. No error, no trap.
// CLEAN
// ASAN
#include <stddef.h>

int last_is_end(const int *p, size_t n) {
  int s = 0;
  for (size_t i = 0; i < n; i++) s += p[i];
  return p[n] == -1 && s > 0; // TRUSTED: spatial:caller-contract
}

int main(void) {
  int a[4] = {1, 2, 3, -1};
  return last_is_end(a, 3) ? 0 : 1;
}
