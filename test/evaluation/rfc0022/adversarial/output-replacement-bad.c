/* RFC 0022: distinct evidence and invalidation.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include <stdlib.h>
static void create(char **p) {
  *p = malloc(1);
  if (*p)
    **p = 7;
}
int main(void) {
  char *p = 0;
  create(&p);
  free(p);
  p = malloc(1);
  if (!p)
    return 0;
  int n = *p;
  free(p);
  return n;
}
