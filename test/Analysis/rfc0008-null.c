// RFC 0008, *Nullness*: a pointer that is null or may be null on some path
// is not dereferenced, nor passed to a callee that dereferences it. Testing
// the pointer, or its callee's outcome, clears the fact on the surviving edge.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
// RUN: not %weavec --dump-analysis %s -- 2>&1 | FileCheck --check-prefix=DUMP %s
#include <stdlib.h>
#include <string.h>
#include <weavec.h>

struct node {
  int value;
  struct node *next;
};

// The RFC's snippets that must be reported.

int unchecked(void) {
  struct node *n = malloc(sizeof *n);
  // CHECK: rfc0008-null.c:[[@LINE+2]]:3: error: dereference of 'n', which may be null [weavec::null-dereference]
  // CHECK: rfc0008-null.c:[[@LINE-2]]:20: note: 'n' may be null: it is the result of 'malloc' here
  n->value = 1;
  free(n);
  return 0;
}

int constant(void) {
  struct node *n = NULL;
  // CHECK: rfc0008-null.c:[[@LINE+2]]:10: error: dereference of 'n', which is null [weavec::null-dereference]
  // CHECK: rfc0008-null.c:[[@LINE-2]]:20: note: 'n' is assigned NULL here
  return n->value;
}

int tested_then_merged(struct node *n) {
  if (n == NULL)
    n = malloc(sizeof *n);
  // CHECK: rfc0008-null.c:[[@LINE+2]]:3: error: dereference of 'n', which may be null [weavec::null-dereference]
  // CHECK: rfc0008-null.c:[[@LINE-2]]:9: note: 'n' may be null: it is the result of 'malloc' here
  n->value = 0;
  free(n);
  return 0;
}

// A callee's dereference becomes a `requires` fact; passing a maybe-null
// pointer to it is the error, at the call.
static int value_of(struct node *n) { return n->value; }

int passes_null(void) {
  struct node *n = malloc(sizeof *n);
  // CHECK: rfc0008-null.c:[[@LINE+3]]:20: error: 'n', which may be null, is passed to 'value_of', which dereferences it [weavec::null-dereference]
  // CHECK: rfc0008-null.c:[[@LINE-2]]:20: note: 'n' may be null: it is the result of 'malloc' here
  // CHECK: rfc0008-null.c:[[@LINE-6]]:12: note: 'value_of' is declared here
  int v = value_of(n);
  free(n);
  return v;
}

// A callee that may return null hands the fact to its caller.
static struct node *make(int v) {
  struct node *n = malloc(sizeof *n);
  if (n)
    n->value = v;
  return n;
}

int from_callee(void) {
  struct node *n = make(1);
  // CHECK: rfc0008-null.c:[[@LINE+2]]:11: error: dereference of 'n', which may be null [weavec::null-dereference]
  // CHECK: rfc0008-null.c:[[@LINE-2]]:20: note: 'n' may be null: it is the result of 'make' here
  int v = n->value;
  free(n);
  return v;
}

// A test that merges back leaves the place maybe-null with the test's note.
int tested_then_passed(struct node *n) {
  if (!n) {
    // CHECK: rfc0008-null.c:[[@LINE+3]]:21: error: 'n', which is null, is passed to 'value_of', which dereferences it [weavec::null-dereference]
    // CHECK: rfc0008-null.c:[[@LINE-2]]:8: note: 'n' may be null: it is compared with NULL here
    // CHECK: rfc0008-null.c:[[@LINE-34]]:12: note: 'value_of' is declared here
    return value_of(n);
  }
  return value_of(n);
}

// Annotations (RFC 0008, *Annotations*): `WEAVEC_NULLABLE` on a result makes
// callers test it; `WEAVEC_NONNULL` on a parameter makes callers prove it.
// Neither says anything about ownership, so the RFC 0003 annotations stay.
extern struct node *lookup(int key) WEAVEC_BORROWED WEAVEC_NULLABLE;
extern int consume_nonnull(struct node *n WEAVEC_BORROWED WEAVEC_NONNULL);

int annotated(void) {
  struct node *n = lookup(1);
  // CHECK: rfc0008-null.c:[[@LINE+2]]:10: error: dereference of 'n', which may be null [weavec::null-dereference]
  // CHECK: rfc0008-null.c:[[@LINE-6]]:21: note: 'n' may be null: the result of 'lookup' is declared WEAVEC_NULLABLE here
  return n->value;
}

int annotated_param(struct node *n) {
  if (!n) {
    // CHECK: rfc0008-null.c:[[@LINE+3]]:28: error: 'n', which is null, is passed to 'consume_nonnull', which dereferences it [weavec::null-dereference]
    // CHECK: rfc0008-null.c:[[@LINE-2]]:8: note: 'n' may be null: it is compared with NULL here
    // CHECK: rfc0008-null.c:[[@LINE-13]]:12: note: 'consume_nonnull' is declared here
    return consume_nonnull(n);
  }
  return consume_nonnull(n);
}

// Clean: every idiom for testing a pointer clears the fact on the edge
// where it is non-null, the fact travels with the value (so a copy taken
// before the test is cleared with it), and unknown pointers are trusted.
int clean(struct node *unknown) {
  struct node *a = malloc(sizeof *a);
  if (!a)
    return 1;
  a->value = 1;
  struct node *b = malloc(sizeof *b);
  struct node *b2 = b;
  if (b == NULL) {
    free(a);
    return 1;
  }
  b2->value = a->value;
  struct node *c = malloc(sizeof *c);
  int v = c ? c->value : 0;
  if (c && c->next)
    v += c->next->value;
  char *s = strdup("x");
  if (s != NULL)
    s[0] = 'y';
  free(s);
  free(c);
  free(b);
  free(a);
  return v + unknown->value;
}

// Clean: a redundant test of a pointer already known non-null (cJSON's
// `can_access_at_index` retests on every use) does not make it maybe-null
// once the edges merge; `(T *)0` is a null test like `NULL`; unchecked code
// may store through anything it is handed by address (RFC 0008,
// *Implementation notes*).
struct list {
  unsigned len;
  struct node **items;
};
extern void fill(struct list *out);

int redundant_tests(struct node *n) {
  if (n == NULL || n->next == NULL)
    return 0;
  while (n != (struct node *)0 && n->value > 0 && n->next->value == 0)
    n->value--;
  return n->value;
}

int filled_by_unchecked_code(void) {
  struct list l = {0, NULL};
  // CHECK: rfc0008-null.c:[[@LINE+1]]:3: warning: call to 'fill' is not checked
  fill(&l);
  if (l.len == 0)
    return 0;
  return l.items[0]->value;
}

// A pointer that a callee's outcome makes non-null: the `notnull` fact.
static int open_node(struct node **out) {
  *out = malloc(sizeof **out);
  return *out != NULL;
}

int outcome(void) {
  struct node *n;
  if (!open_node(&n))
    return 1;
  n->value = 2;
  free(n);
  return 0;
}

// A `fresh` store the callee did not perform on its failing class is in
// `null{...}` so the caller drops the record (RFC 0007); it says nothing
// about the value, which is what the caller stored there before.
struct buf {
  char *data;
  unsigned len;
};

static int grow(struct buf *b, unsigned n) {
  if (n <= b->len)
    return 0;
  char *p = realloc(b->data, n);
  if (p == NULL)
    return -1;
  b->data = p;
  b->len = n;
  return 0;
}

void truncate_to(struct buf *b, unsigned n) {
  if (grow(b, n) == -1 && n > b->len)
    n = b->len;
  b->data[n] = 0;
}

// The summary vocabulary: `requires{...}`, `null` among the returns, and
// `null{...}` / `notnull{...}` per outcome class (RFC 0008, *Summary text
// format*).
// DUMP: function 'value_of':
// DUMP: summary: n->value: read; stores{} returns{} requires{n}
// DUMP: function 'make':
// DUMP: summary: stores{} returns{fresh(free), null}
// DUMP: function 'redundant_tests':
// DUMP: summary: n->next: read; n->next->value: read; n->value: read|written; stores{} returns{}
// DUMP: function 'open_node':
// DUMP: summary: *out: read|written; stores{*out = fresh(free), *out = null} returns{} requires{out} outcome zero{} null{*out} outcome positive{} notnull{*out}
// DUMP: function 'grow':
// DUMP: summary: b->data: written|moved(free)|replaced; b->len: read|written; stores{b->data = fresh(free)} returns{} requires{b} outcome zero{b->data: moved(free) replaced} stored{b->data} outcome negative{} null{b->data} stored{}
// DUMP: function 'truncate_to':
// DUMP-NOT: maybe-null
// DUMP: summary: b->data: read|written|moved(free)|replaced;

// CHECK: 1 warning and 8 errors generated.
