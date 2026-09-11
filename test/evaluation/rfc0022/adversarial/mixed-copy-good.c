/* RFC 0022: every reaching target must establish its obligations. */
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
  (void)argv;
  void *(*fn)(void *, const void *, size_t) = argc > 1 ? memcpy : memmove;
  char s[4] = {1, 2, 3, 4};
  char d[4];
  fn(d, s, 4);
  return d[3];
}
