// RFC 0030 §7.5: a loop that runs zero times accesses nothing, so f(NULL, 0) meets its requirement.
// STAGE: S6
// R2 gives 'clear_tail' the requirement 0 < n -> Counted(n + 1) and 0 < n -> nonnull. Each
// call checks both parts under the guard, so clear_tail(NULL, 0) runs no check and
// clear_tail(a, 2) needs three elements, which 'a' has. No error, no trap.
// CLEAN
// ASAN
#include <stddef.h>

static void clear_tail(int *p, size_t n) {
  for (size_t i = 0; i < n; ++i) p[i + 1] = 0;
}

int main(int argc, char **argv) {
  int a[3] = {1, 2, 3};
  (void)argv;
  clear_tail(NULL, (size_t)argc - 1);
  clear_tail(a, 2);
  return a[0] == 1 && a[1] == 0 && a[2] == 0 ? 0 : 1;
}
