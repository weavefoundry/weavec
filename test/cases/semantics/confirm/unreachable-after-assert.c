// RFC 0034 section 6 (confirmed errors): a false definite null dereference
// the milestone found in QuickJS's quickjs.check.o build
// (build/eval-2026-10-04/repros/quickjs-2.c). The DIRECT branch ends in an
// assert that cannot hold, so b->allowed is unreachable: the else edge
// excludes DIRECT from eval_type (section 6.2), so the second test's true
// edge is infeasible in every run, the replay's too. An unconfirmed
// candidate may remain a warning (RFC 0034, Diagnostics), hence the ALLOW.
// CLEAN
// ALLOW: null-dereference
/* QuickJS __JS_EvalInternal (quickjs.c:37449-37484) in CONFIG_CHECK_JSVALUE
 * mode: the DIRECT branch ends in an assert that can never hold there, so
 * only the else branch (b = NULL, eval_type != DIRECT) survives; the later
 * `if (eval_type == DIRECT) ... b->x` is then unreachable, but weavec-cc
 * reports a definite null-dereference of 'b' (it does not keep
 * eval_type != DIRECT on the surviving path). */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef struct B { int allowed; } B;
typedef struct F { B *bc; uintptr_t val; } F;
#define DIRECT 1
#ifndef NEGATIVE
#define TAG_OK(v) (((v) & 0xf) == (uintptr_t)-1) /* never true */
#else
#define TAG_OK(v) (((v) & 0xf) == 0xf)           /* negative: can hold */
#endif

int eval(int flags, F *cur) {
  int eval_type = flags & 3;
  B *b;
  if (eval_type == DIRECT) {
    assert(TAG_OK(cur->val));
    b = cur->bc;
  } else {
    b = NULL;
  }
  printf("eval %d\n", eval_type);
  if (eval_type == DIRECT)
    return b->allowed;
  return 0;
}

int main(void) {
  F f = {0, 0};
  return eval(0, &f); /* eval_type 0: never takes the DIRECT path */
}
