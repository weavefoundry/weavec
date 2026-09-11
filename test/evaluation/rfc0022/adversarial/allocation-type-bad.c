/* RFC 0022: distinct evidence and invalidation.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include <stdlib.h>
int main(void) {
  int *p = malloc(sizeof(int));
  if (!p)
    return 0;
  *p = 7;
  void *v = p;
  float *q = v;
  int n = *q;
  free(p);
  return n;
}
