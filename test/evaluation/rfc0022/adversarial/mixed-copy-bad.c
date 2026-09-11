/* RFC 0022: every reaching target must establish its obligations. */
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
  (void)argv;
  void *(*fn)(void *, const void *, size_t) = argc > 1 ? memcpy : memmove;
  char s[4] = {1, 2, 3, 4};
  fn(s + 1, s, 3);
  return s[3];
}
