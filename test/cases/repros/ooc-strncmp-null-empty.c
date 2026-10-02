// Held-out repro (RFC 0031 Motivation, §9.1, §11.1): mujs regexp.c:1174 (match, I_REF)
// compares a back-reference with 'strncmp(sp, out->sub[n].sp, i)', where
// 'i = ep - sp' of the capture; a group that did not participate in the match has
// 'sp == ep == NULL', so the call is 'strncmp(s, NULL, 0)' (e.g. /(a)?b\1/ on "b").
// strncmp with a zero count reads neither string.
// v0.11.0's library table requires both strncmp arguments to be non-null whatever the
// count, so the null facet is checked and the correct program traps (nonnull) at run time.
// intended: no finding and no trap (the zero-length form of the library row).
// RUN-INPUT:
// RUN-INPUT: 1
// CLEAN
// ASAN
#include <stddef.h>
#include <string.h>
struct sub { const char *sp, *ep; };
static int backref(const char *sp, const struct sub *s) {
  int i = (int)(s->ep - s->sp);
  if (strncmp(sp, s->sp, (size_t)i))
    return 1;
  return 0;
}
int main(int argc, char **argv) {
  (void)argv;
  static const char text[] = "aba";
  struct sub s = { NULL, NULL };
  if (argc > 1) { s.sp = text; s.ep = text + 1; }
  return backref(text + 2, &s);
}
