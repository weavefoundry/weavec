/* RFC 0019: consume the initialized prefix of the encoding interface.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include "utf.h"
int main(void) {
  char bytes[4];
  size_t size;
  if (utf8_encode(0x20ac, bytes, &size) != 0)
    return 0;
  /* U+20AC has three encoded bytes. The call contract must establish all
   * three, and must also initialize the separately returned byte count. */
  return utf8_check_first(bytes[0]) == 3 && bytes[2] != 0 && size == 3 ? 0 : 1;
}
