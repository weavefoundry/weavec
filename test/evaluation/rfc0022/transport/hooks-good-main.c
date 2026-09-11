/* RFC 0022: checked interfaces across translation units.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include <stdlib.h>
void setup(void *(*)(size_t), void (*)(void *));
void *acquire(size_t);
void drop(void *);
static void *short_allocate(size_t n) {
  (void)n;
  return malloc(1);
}
int main(void) {
  setup(malloc, free);
  char *p = acquire(8);
  if (!p)
    return 0;
  p[7] = 7;
  drop(p);
  return 0;
}
