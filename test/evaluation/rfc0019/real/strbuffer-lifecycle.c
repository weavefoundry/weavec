/* RFC 0019: unmodified Jansson strbuffer lifecycle caller.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include "strbuffer.h"
#include <stdlib.h>
#include <string.h>
int main(void) {
  strbuffer_t buffer;
  if (strbuffer_init(&buffer) != 0)
    return 0;
  if (strbuffer_append_bytes(&buffer, "abcdefghijklmnop", 16) != 0) {
    strbuffer_close(&buffer);
    return 0;
  }
  if (strbuffer_append_byte(&buffer, 'q') != 0) {
    strbuffer_close(&buffer);
    return 0;
  }
  char last = strbuffer_pop(&buffer);
  const char *value = strbuffer_value(&buffer);
  int valid = last == 'q' && strlen(value) == 16;
  strbuffer_clear(&buffer);
  char *owned = strbuffer_steal_value(&buffer);
  valid &= owned[0] == 0;
  free(owned);
  strbuffer_close(&buffer);
  return valid ? 0 : 1;
}
