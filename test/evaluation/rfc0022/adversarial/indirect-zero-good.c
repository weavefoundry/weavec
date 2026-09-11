/* RFC 0022: known indirect memset preserves its actual byte value. */
#include <string.h>
int main(void) {
  void *(*fill)(void *, int, size_t) = memset;
  char buffer[4];
  fill(buffer, 0, sizeof buffer);
  return (int)strlen(buffer);
}
