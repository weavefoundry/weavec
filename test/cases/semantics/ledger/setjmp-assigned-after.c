// RFC 0030 §5.4: values may be stale after a longjmp.
// STAGE: S7
// `p` is assigned after `setjmp` and read on its second return (mujs's
// `js_try` blocks, which free a buffer the protected code allocated). A
// `longjmp` from inside `xalloc` returns there before the assignment, one
// from later code after it; a path through the first return sees neither.
// Reading `p` there was a definite (and false) `use-of-uninitialized`; an
// unwritten local of a function that calls `setjmp` is now only possibly
// unassigned. (The allocation `fail` jumps away from is freed by the
// handler, which the model does not follow from the `longjmp`: a leak.)
// CLEAN
// ALLOW: leak
// RUN-INPUT:
#include <setjmp.h>
#include <stdlib.h>

static jmp_buf env;

static void *xalloc(size_t n) {
  void *p = malloc(n);
  if (!p)
    longjmp(env, 1);
  return p;
}

static void fail(void) { longjmp(env, 2); }

static int work(size_t n) {
  char *p = NULL;
  char *q;
  if (setjmp(env)) {
    free(q);
    return -1;
  }
  q = xalloc(n);
  p = q;
  p[0] = 1;
  fail();
  return 0;
}

int main(void) { return work(4) == -1 ? 0 : 1; }
