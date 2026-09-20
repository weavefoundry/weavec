// Guard checked against a global length; a helper shrinks the buffer before the access.
// ASAN
#include <stdlib.h>
static char *g_buf;
static size_t g_len;
static void shrink(void) {
  char *n = realloc(g_buf, 2);
  if (n) g_buf = n;
}
int main(void) {
  g_buf = calloc(64, 1);
  if (!g_buf) return 1;
  g_len = 64;
  size_t i = 40;
  if (i < g_len) {
    shrink();
    int r = g_buf[i]; // BUG: out-of-bounds // NOT-PROVEN: spatial
    free(g_buf);
    return r;
  }
  free(g_buf);
  return 0;
}
