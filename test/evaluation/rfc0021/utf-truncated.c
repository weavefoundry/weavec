/* RFC 0021: the requested three-byte sequence exceeds its object. */
#include "utf.h"
int main(void) {
  const char sequence[2] = {(char)0xe1, (char)0x80};
  int32_t codepoint;
  return (int)utf8_check_full(sequence, 3, &codepoint);
}
