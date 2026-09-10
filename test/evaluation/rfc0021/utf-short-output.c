/* RFC 0021: four-byte UTF output cannot fit one byte. */
#include "utf.h"
int main(void) {
  char encoded[1];
  size_t size;
  return utf8_encode(0x10000, encoded, &size);
}
