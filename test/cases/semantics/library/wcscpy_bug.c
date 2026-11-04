// RFC 0037 §5: a wide string is scanned by the runtime, in elements.
// RUN-INPUT:
#include <stdlib.h>
#include <wchar.h>

int main(void) {
  wchar_t *d = malloc(4 * sizeof(wchar_t));
  wcscpy(d, L"0123456789"); // TRAP: heap-buffer-overflow
  return (int)d[0];
}
