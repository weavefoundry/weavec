/* RFC 0022: distinct evidence and invalidation.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include <stdlib.h>
static void *(*allocate)(size_t) = malloc;
static void setup(void *(*a)(size_t)) {
  allocate = a;
}
int main(void) {
  setup(0);
  char *p = allocate(1);
  if (p)
    free(p);
  return 0;
}
