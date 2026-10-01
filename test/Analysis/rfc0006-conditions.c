// RFC 0006, *Condition facts on CFG edges*: pointer equality tests refine
// the alias relation on the edge they hold on, and `!=` separates only
// exact aliases (pointer arithmetic makes a copy interior).
// RUN: not %weavec --ledger=%t.json %s -- 2>&1 | FileCheck %s
// RUN: FileCheck --check-prefix=LEDGER %s < %t.json
// RUN: not %weavec --dump-analysis %s -- 2>&1 | FileCheck --check-prefix=DUMP %s
#include "../Inputs/prelude.h"

struct list {
  struct list *next;
};

static char *get(void) { return malloc(4); }
extern char *sentinel_value;

// Clean: on the `!=` edge the two pointers are known to be distinct.
// The object engine does not refine the fresh `l` to null on the `==` edge
// (a fresh object is distinct from what a global holds, RFC 0031 §4.5 D4,
// so only null can compare equal), and reports a leak there: a possible
// finding on correct code (RFC 0031 *Accepted false positives*).
void sentinel(void) {
  char *l = get();
  if (l == sentinel_value)
    // CHECK: rfc0006-conditions.c:[[@LINE+1]]:5: warning: 'l' is leaked [weavec::leak]
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
// Format 30 (RFC 0031 §6.1) has no value for "a fresh block or the entry
// value of a global", so `feed`'s result, and with it `read_line`'s, is
// unknown: the callers' accesses are unresolved rather than proven (a loss
// of precision, not of a finding).
// DUMP-LABEL: function 'feed':
// DUMP: result unknown maybe-null when null nonnull
char *read_line(void) {
  char *res;
  while ((res = feed()) == sentinel_value)
    ;
  return res;
}
// DUMP-LABEL: function 'read_line':
// DUMP: result unknown maybe-null when null nonnull
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
    // The object engine remembers `p == q` (RFC 0031 *Implementation
    // amendments*, *Pointer comparisons*) but does not use it to decide the
    // temporal facet of `q`: not proven, no longer definite
    // (test/cases/KNOWN-DIFFERENCES.md, *Lit tests*).
    // LEDGER: "line": [[@LINE+7]],
    // LEDGER-NEXT: "column": 5,
    // LEDGER-NEXT: "text": "use(q)",
    // LEDGER: "facets": {
    // LEDGER-NEXT: "temporal": {
    // LEDGER-NEXT: "outcome": "unresolved",
    // LEDGER-NEXT: "reason": "may-alias-released",
    use(q);
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

// CHECK: 1 warning and 2 errors generated.
