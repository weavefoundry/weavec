/* RFC 0022: checked interfaces across translation units.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include <stdlib.h>
static void create(char **out) {
  *out = malloc(1);
}
void invoke(char **out) {
  create(out);
}
