/* RFC 0022: frozen checked interface evaluation.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include <stdlib.h>
static void *allocate(size_t n) {
  return malloc(n);
}
static void release(void *p) {
  free(p);
}
int main(void) {
  void *(*fn)(size_t) = allocate;
  char *p = fn(8);
  if (!p)
    return 0;
  p[7] = 1;
  release(p);
  return 0;
}
