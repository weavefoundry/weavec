// Root-cause repro zerolen: write(fd, NULL, 0) on an empty append buffer (linenoise's
// refresh); v0.10.0 reports ''ab.b', which may be null, is passed to 'write'' (false).
// intended (RFC 0030 section 8.3, gate S4): write's buffer is null-if-zero, no finding.
// CLEAN
// ASAN
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
struct abuf { char *b; int len; };
static void ab_append(struct abuf *ab, const char *s, int len) { char *n = realloc(ab->b, (size_t)(ab->len + len)); if (!n) return; memcpy(n + ab->len, s, (size_t)len); ab->b = n; ab->len += len; }
void refresh(int fd, int c) { struct abuf ab = {NULL, 0}; if (c) ab_append(&ab, "x", 1); if (write(fd, ab.b, (size_t)ab.len) == -1) {} free(ab.b); }
// Driver added in the import (RFC 0030 section 17.2) so the executable oracle runs it.
int main(void) { refresh(1, 0); refresh(1, 1); return 0; }
