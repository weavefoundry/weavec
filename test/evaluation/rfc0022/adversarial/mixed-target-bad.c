/* RFC 0022: distinct evidence and invalidation.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include <stdlib.h>
static void *custom(size_t n) {
  (void)n;
  return malloc(1);
}
int main(int argc, char **argv) {
  (void)argv;
  void *(*a)(size_t) = argc > 1 ? custom : malloc;
  char *p = a(8);
  if (!p)
    return 0;
  p[7] = 1;
  free(p);
  return 0;
}
