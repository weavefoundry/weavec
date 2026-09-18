#include <string.h>
int inspect(const unsigned char *p, unsigned n, unsigned mode) {
  if (mode == 0 && strncmp((const char *)p, "\xef\xbb\xbf", 3) == 0) return p[n];
  if (mode == 1 && memcmp(p, "\0y", 2) >= 0) return p[n];
  if (mode == 2 && strcmp((const char *)p, "\0y") != 0) return p[n];
  if (mode == 3 && memcmp(p, "a", 1) <= 0) return p[n];
  if (mode == 4 && strncmp((const char *)p, "abc", 3) != 0) return p[n];
  if (mode == 5 && memcmp(p, "abc", 3) == 1) return p[n];
  return 0;
}
