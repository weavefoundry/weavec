/* RFC 0022: frozen checked interface evaluation.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include <stdlib.h>
static void invoke(void (*fn)(void *), void *p) {
  fn(p);
}
int main(void) {
  char *p = malloc(1);
  if (!p)
    return 0;
  *p = 1;
  invoke(free, p);
  return *p;
}
