/* RFC 0022: distinct evidence and invalidation.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include <stdlib.h>
int main(int argc, char **argv) {
  (void)argv;
  int x = 7;
  float y = 7;
  void *v = argc > 1 ? (void *)&x : (void *)&y;
  return *(int *)v;
}
