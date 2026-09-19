// RFC 0030 §7.5: requirement terms are mathematical integers, so a size_t 'i - 1' loop does not wrap.
// STAGE: S6
// 'for (i = 1; i < n; i++) ... p[i - 1]' gives 1 < n -> Counted(n - 1) (R2 with k = -1).
// With n == 0 or 1 the guard is false; the term n - 1 is never a wrapped size_t count, and
// a count of zero or less is no requirement. The calls pass (NULL, 0), (a, 1) and (a, 3).
// No error, no trap.
// CLEAN
// ASAN
#include <stddef.h>

static int sum_prev(const int *p, size_t n) {
  int s = 0;
  for (size_t i = 1; i < n; i++) s += p[i - 1];
  return s;
}

int main(int argc, char **argv) {
  int a[3] = {1, 2, 3};
  size_t none = (size_t)argc - 1;
  (void)argv;
  return sum_prev(NULL, none) == 0 && sum_prev(a, 1) == 0 && sum_prev(a, 3) == 3 ? 0 : 1;
}
