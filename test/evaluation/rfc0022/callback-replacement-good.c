/* RFC 0022: frozen checked interface evaluation.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include <stdlib.h>
int main(void) {
  char *p = malloc(2);
  if (!p)
    return 0;
  p[0] = 7;
  void *(*resize)(void *, size_t) = realloc;
  char *q = resize(p, 4);
  if (!q) {
    free(p);
    return 0;
  }
  int n = q[0];
  free(q);
  return n;
}
