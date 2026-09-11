/* RFC 0022 negative: an extracted allocation ends when its owner frees it.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include "jansson.h"
#include "strbuffer.h"

#include <stdlib.h>
int main(void) {
  json_set_alloc_funcs2(malloc, realloc, free);
  strbuffer_t buffer;
  if (strbuffer_init(&buffer) != 0)
    return 0;
  char *value = strbuffer_steal_value(&buffer);
  free(value);
  strbuffer_close(&buffer);
  return value[0];
}
