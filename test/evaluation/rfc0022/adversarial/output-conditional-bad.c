/* RFC 0022: distinct evidence and invalidation.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include <stdlib.h>
static void create(char **p, int fill) {
  *p = malloc(1);
  if (*p && fill)
    **p = 7;
}
int main(int argc, char **argv) {
  (void)argv;
  char *p = 0;
  create(&p, argc > 1);
  if (!p)
    return 0;
  int n = *p;
  free(p);
  return n;
}
