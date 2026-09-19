// Variadic misuse: callee reads more arguments than passed, and reads as wrong type.
// ASAN
#include <stdarg.h>
static int sum_ptrs(int n, ...) {
  va_list ap;
  va_start(ap, n);
  int s = 0;
  for (int i = 0; i < n; i++) { int *p = va_arg(ap, int *); s += *p; } // BUG: out-of-bounds // NOT-PROVEN: spatial
  va_end(ap);
  return s;
}
int main(void) {
  int a = 1;
  return sum_ptrs(3, &a, 2);
}
