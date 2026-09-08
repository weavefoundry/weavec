/* RFC 0019 negative: four output bytes require four bytes of capacity.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include "utf.h"
int main(void) {
  char bytes[3];
  size_t size;
  return utf8_encode(0x10000, bytes, &size);
}
