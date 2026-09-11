/* RFC 0022: frozen checked interface evaluation.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include <stdlib.h>
struct hooks {
  void *(*allocate)(size_t);
  void (*release)(void *);
};
static struct hooks h = {malloc, free};
int main(void) {
  char *p = h.allocate(8);
  if (!p)
    return 0;
  p[8] = 1;
  h.release(p);
  return 0;
}
