// RFC 0006, *Condition facts on CFG edges*: pointer equality tests refine
// the alias relation on the edge they hold on, and `!=` separates only
// exact aliases (pointer arithmetic makes a copy interior).
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
// RUN: not %weavec --dump-analysis %s -- 2>&1 | FileCheck --check-prefix=DUMP %s
#include "../Inputs/prelude.h"

struct list {
  struct list *next;
};

static char *get(void) { return malloc(4); }
extern char *sentinel_value;

// Clean: on the `!=` edge the two pointers are known to be distinct.
void sentinel(void) {
  char *l = get();
  if (l == sentinel_value)
    return;
  free(l);
  use(sentinel_value);
}

void not_equal_then_free(char *p, char *q) {
  if (p != q) {
    free(p);
    use(q);
  }
}

void unlink(struct list *head, struct list *victim) {
  struct list *cur = head;
  if (cur != victim) {
    free(victim);
    use(cur);
  }
}

// The linenoise idiom: a reader that returns either a fresh line or a
// sentinel global, looped on until it does not; the exit edge separates.
int ready(void);
static char *feed(void) { return ready() ? get() : sentinel_value; }
// DUMP: function 'feed':
// DUMP: summary: stores{} returns{fresh(free), copy sentinel_value, null}
char *read_line(void) {
  char *res;
  while ((res = feed()) == sentinel_value)
    ;
  return res;
}
// DUMP: function 'read_line':
// DUMP: summary: stores{} returns{fresh(free), null}
void reader_loop(void) {
  for (;;) {
    char *line = read_line();
    if (line == NULL)
      break;
    line[0] = 0;
    free(line);
  }
}

// Reported: on the `==` edge the two are the same object.
void equal_then_free(char *p, char *q) {
  if (p == q) {
    free(p);
    // CHECK: rfc0006-conditions.c:[[@LINE+1]]:9: error: use of 'q' after it was freed [weavec::use-after-free]
    use(q);
    // CHECK: rfc0006-conditions.c:[[@LINE-3]]:5: note: freed here (through 'p')
  }
}

// Reported: `q = p + 1` is an interior alias; `!=` does not separate it.
void interior_alias(char *p) {
  char *q = p + 1;
  if (p != q) {
    free(p);
    // CHECK: rfc0006-conditions.c:[[@LINE+1]]:9: error: use of 'q' after it was freed [weavec::use-after-free]
    use(q);
  }
}

// Reported: a copy is exact and is separated, but the join after the `if`
// restores the may-alias.
void separated_then_joined(char *p, char *q) {
  char *r = p;
  if (r != q)
    use(q);
  free(p);
  // CHECK: rfc0006-conditions.c:[[@LINE+1]]:7: error: use of 'r' after it was freed [weavec::use-after-free]
  use(r);
}

// CHECK: 3 errors generated.
