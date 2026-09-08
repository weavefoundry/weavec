/* RFC 0019 negative: a rejected codepoint establishes no output bytes.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include "utf.h"
int main(void) {
  char bytes[4];
  size_t size;
  (void)utf8_encode(-1, bytes, &size);
  return bytes[0];
}
