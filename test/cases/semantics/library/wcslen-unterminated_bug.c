// RFC 0037 §5: a wide string with no terminator in its block is reported
// as it is scanned, each element checked before it is read.
// RUN-INPUT:
#include <stdlib.h>
#include <wchar.h>

int main(void) {
  wchar_t *s = malloc(4 * sizeof(wchar_t));
  for (int i = 0; i < 4; ++i)
    s[i] = L'a';
  return (int)wcslen(s); // TRAP: unterminated-string
}
