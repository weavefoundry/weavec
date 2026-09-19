// CLEAN
// ASAN
#include <stdarg.h>
static int sum_ptrs(int n, ...) {
  va_list ap;
  va_start(ap, n);
  int s = 0;
  for (int i = 0; i < n; i++) { int *p = va_arg(ap, int *); s += *p; }
  va_end(ap);
  return s;
}
int main(void) {
  int a = 1, b = 2, c = 3;
  return sum_ptrs(3, &a, &b, &c);
}
