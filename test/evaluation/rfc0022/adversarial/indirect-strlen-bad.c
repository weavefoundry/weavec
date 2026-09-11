/* RFC 0022: distinct evidence and invalidation.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include <stdlib.h>
#include <string.h>
int main(void) {
  size_t (*length)(const char *) = strlen;
  char x[2] = {7, 7};
  return length(x);
}
