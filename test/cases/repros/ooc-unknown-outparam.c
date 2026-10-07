// Held-out repro (RFC 0031 Motivation, §11.1): hiredis test.c:292-350 formats a command into
// 'cmd' with the external 'redisFormatCommand(&cmd, ...)', frees it, and formats again into
// the same variable: 'fmt(&cmd, ...); free(cmd); fmt(&cmd, ...); free(cmd);'.
// v0.11.0 reports definite 'use-after-free' and 'double-free' errors on the second use and
// release: the write through '&cmd' by an unknown callee is not modelled, so 'cmd' still
// holds the freed value. Reduced from build/rfc31/ooc/repro/outp.c; the formatters are
// defined in a unit the analysis does not see (Inputs/ooc-unknown-outparam-impl.c).
// intended: no finding; an unknown callee given '&cmd' may overwrite 'cmd' with a fresh
// value, so the second use and release are of that value.
// UNITS: Inputs/ooc-unknown-outparam-impl.c
// CLEAN
// The formatters' unit has no record, which the link step says (RFC 0030 §13.2).
// ASAN
#include <stdlib.h>
#include <string.h>
int fmtv(char **out, const char *f, ...);
int fmt1(char **out, const char *f);
void hi_free(void *p);
int a(void) {
  char *cmd;
  int r = 0;
  int n = fmtv(&cmd, "a");
  if (n < 0) return -1;
  if (strncmp(cmd, "x", (size_t)n) == 0) r++;
  free(cmd);
  n = fmtv(&cmd, "b");
  if (n < 0) return -1;
  if (strncmp(cmd, "x", (size_t)n) == 0) r++;
  free(cmd);
  return r;
}
void b(void) {
  char *cmd;
  if (fmt1(&cmd, "a") < 0) return;
  free(cmd);
  if (fmt1(&cmd, "b") < 0) return;
  free(cmd);
}
void c(void) {
  char *cmd;
  if (fmtv(&cmd, "a") < 0) return;
  hi_free(cmd);
  if (fmtv(&cmd, "b") < 0) return;
  hi_free(cmd);
}
int main(void) {
  int r = a();
  b();
  c();
  return r == 0 ? 0 : 1;
}
