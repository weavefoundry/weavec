// Link-only unit of ooc-unknown-outparam.c: the definitions the analysis must not see
// (hiredis's redisFormatCommand and hi_free live in another library), compiled as plain
// Clang so the executable links.
// FLAGS: -fno-weavec
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
static int make(char **out, const char *f) {
  size_t n = strlen(f);
  char *s = malloc(n + 1);
  if (!s) { *out = NULL; return -1; }
  memcpy(s, f, n + 1);
  *out = s;
  return (int)n;
}
int fmtv(char **out, const char *f, ...) { return make(out, f); }
int fmt1(char **out, const char *f) { return make(out, f); }
void hi_free(void *p) { free(p); }
