/* RFC 0021: supplementary closed caller for the unchanged UTF interfaces. */
#include "utf.h"

int main(void) {
  char encoded[4];
  size_t size;
  int32_t codepoint;
  const char sequence[2] = {(char)0xc2, (char)0xa2};
  if (utf8_encode(0xa2, encoded, &size) != 0)
    return 0;
  if (size != 2 || utf8_check_first(encoded[0]) != 2)
    return 0;
  if (!utf8_check_full(sequence, 2, &codepoint))
    return 0;
  if (codepoint != 0xa2)
    return 0;
  const char *end = utf8_iterate(sequence, 2, &codepoint);
  if (!end)
    return 0;
  if (end - sequence != 2)
    return 0;
  return utf8_check_string(sequence, 2);
}
