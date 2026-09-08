/* RFC 0019 negative: the declared size exceeds the allocation.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include "strbuffer.h"
#include <stdlib.h>
int main(void) {
  char *value = malloc(1);
  if (!value) return 0;
  value[0] = 0;
  strbuffer_t buffer = {value, 0, 16};
  int result = strbuffer_append_bytes(&buffer, "ab", 2);
  free(buffer.value);
  return result;
}
