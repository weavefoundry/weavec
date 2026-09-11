/* RFC 0022: frozen checked interface evaluation.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include <stdlib.h>
static void create(char **out) {
  *out = malloc(1);
  if (*out)
    **out = 7;
}
static void invoke(void (*fn)(char **), char **out) {
  fn(out);
}
int main(void) {
  char *p = 0;
  invoke(create, &p);
  if (!p)
    return 0;
  int n = *p;
  free(p);
  return n;
}
