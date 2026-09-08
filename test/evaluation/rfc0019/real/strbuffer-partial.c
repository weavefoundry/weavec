/* RFC 0019 negative: a logical length does not initialize the bytes.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include "strbuffer.h"
#include <stdlib.h>
int main(void) {
  char *value = malloc(16);
  if (!value) return 0;
  strbuffer_t buffer = {value, 1, 16};
  int result = strbuffer_pop(&buffer);
  strbuffer_close(&buffer);
  return result;
}
