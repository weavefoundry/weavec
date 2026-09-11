/* RFC 0022: distinct evidence and invalidation.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include <stdlib.h>
int main(void) {
  void *v = malloc(sizeof(int));
  if (!v)
    return 0;
  int *q = v;
  int n = *q;
  free(v);
  return n;
}
