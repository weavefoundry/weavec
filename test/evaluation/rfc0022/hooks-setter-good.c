/* RFC 0022: frozen checked interface evaluation.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include <stdlib.h>
static void *(*allocate)(size_t);
static void (*release)(void *);
static void setup(void *(*a)(size_t), void (*f)(void *)) {
  allocate = a;
  release = f;
}
int main(void) {
  setup(malloc, free);
  char *p = allocate(8);
  if (!p)
    return 0;
  p[7] = 1;
  release(p);
  return 0;
}
