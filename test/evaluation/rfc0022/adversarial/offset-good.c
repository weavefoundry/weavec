/* RFC 0022: distinct evidence and invalidation.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include <stdlib.h>
int main(void) {
  int x[2] = {1, 7};
  void *v = &x[1];
  int *p = v;
  return *p;
}
