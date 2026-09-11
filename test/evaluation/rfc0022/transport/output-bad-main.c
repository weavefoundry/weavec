/* RFC 0022: checked interfaces across translation units.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include <stdlib.h>
void invoke(char **);
int main(void) {
  char *p = 0;
  invoke(&p);
  if (!p)
    return 0;
  int x = *p;
  free(p);
  return x;
}
