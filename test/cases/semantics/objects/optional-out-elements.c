// RFC 0031 §4.9, *Summaries*: element stores through an optional output.
// STAGE: S7
// `clear` writes through `sub`, which points to the caller's record or to
// a local one (`if (!sub) sub = &scratch`, mujs's `regexec`): its stores are
// weak, each element's field its entry value or the null stored. The
// summary said "some elements of `sub[*].sp` keep their value", without the
// null, and the caller applied it at the position of `ep`: `m.sub[0].sp`
// stayed uninitialised, a definite (and false) `use-of-uninitialized`.
// CLEAN
// ASAN
// RUN-INPUT:
#include <stddef.h>

typedef struct Resub {
  int nsub;
  struct {
    const char *sp;
    const char *ep;
  } sub[16];
} Resub;

static int clear(int nsub, Resub *sub) {
  Resub scratch;
  if (!sub)
    sub = &scratch;
  sub->nsub = nsub;
  for (int i = 0; i < 16; ++i)
    sub->sub[i].sp = sub->sub[i].ep = NULL;
  return 0;
}

int main(void) {
  Resub m;
  if (clear(1, &m) != 0)
    return 1;
  clear(1, NULL);
  return m.sub[0].sp == NULL && m.sub[15].ep == NULL ? 0 : 1;
}
