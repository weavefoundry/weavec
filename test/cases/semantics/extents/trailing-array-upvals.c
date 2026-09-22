// RFC 0030 §7.4: a trailing array is flexible whatever its bound (Lua's 'UpVal *upvals[1]').
// STAGE: S3
// At -fstrict-flex-arrays=0 every trailing array is flexible. Its extent is the rest of the
// allocation when that is known, and otherwise unresolved(unknown-extent); it never comes
// from the declared bound [1]. The closure has three upvalues, indexed up to 'nupvalues':
// no error, no check against 1, no trap.
// CLEAN
// ASAN
#include <stddef.h>
#include <stdlib.h>

typedef struct UpVal { int v; } UpVal;
typedef struct LClosure { int nupvalues; UpVal *upvals[1]; } LClosure;

static LClosure *newclosure(int n) {
  LClosure *c = malloc(offsetof(LClosure, upvals) + sizeof(UpVal *) * (size_t)n);
  if (c == NULL) return NULL;
  c->nupvalues = n;
  for (int i = 0; i < n; i++) c->upvals[i] = NULL;
  return c;
}

int sumupvals(const LClosure *cl) {
  int s = 0;
  for (int i = 0; i < cl->nupvalues; i++)
    if (cl->upvals[i] != NULL) s += cl->upvals[i]->v; // UNRESOLVED: spatial:unknown-extent
  return s;
}

int main(void) {
  static UpVal u[3] = {{1}, {2}, {3}};
  LClosure *c = newclosure(3);
  if (c == NULL) return 1;
  for (int i = 0; i < 3; i++) c->upvals[i] = &u[i];
  int s = sumupvals(c);
  free(c);
  return s == 6 ? 0 : 1;
}
