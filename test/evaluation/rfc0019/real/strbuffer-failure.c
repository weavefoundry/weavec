/* RFC 0019 negative: failed initialization leaves no allocated value.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include "strbuffer.h"
int main(void) {
  strbuffer_t buffer;
  if (strbuffer_init(&buffer) != 0)
    return buffer.value[0];
  strbuffer_close(&buffer);
  return 0;
}
