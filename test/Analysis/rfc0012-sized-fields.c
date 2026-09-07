// RFC 0012, *Sized fields*: a pointer field counted by a sibling integer
// field, declared with `WEAVEC_SIZED_BY` or inferred from the stores of the
// unit; loads get the count's extent, stores into an annotated field are
// checked against the count, a malformed annotation is reported once.
// RUN: not %weavec %s -- -ferror-limit=0 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"
#include <weavec.h>

char *strcpy(char *dst, const char *src);

struct buf {
  char *WEAVEC_SIZED_BY(cap) data;
  size_t cap;
};

// -- Loads --------------------------------------------------------------------

void put(struct buf *b) {
  // CHECK: rfc0012-sized-fields.c:[[@LINE+1]]:3: error: 'b->data[b->cap]' is out of bounds: 'b->cap' is the number of elements of 'b->data' [weavec::out-of-bounds]
  b->data[b->cap] = 0;
  // CHECK: rfc0012-sized-fields.c:12:30: note: 'b->data' is declared here
  if (b->cap > 0)
    b->data[b->cap - 1] = 0;
}

void fill(struct buf *b) {
  for (size_t i = 0; i < b->cap; i++)
    b->data[i] = 0;
  for (size_t i = 0; i <= b->cap; i++)
    // CHECK: rfc0012-sized-fields.c:[[@LINE+1]]:5: error: 'b->data[i]' may be out of bounds: 'i' may equal 'b->cap', the number of elements of 'b->data' [weavec::out-of-bounds]
    b->data[i] = 0;
}

// The string checks see the count too.
void copy(struct buf *b) {
  if (b->cap == 4)
    // CHECK: rfc0012-sized-fields.c:[[@LINE+1]]:12: error: 'strcpy' accesses 6 bytes of 'b->data', which has 4 bytes [weavec::out-of-bounds]
    strcpy(b->data, "hello");
}

// -- Stores -------------------------------------------------------------------

void shrink(struct buf *b) {
  b->data = malloc(4);
  // CHECK: rfc0012-sized-fields.c:[[@LINE+1]]:3: error: 'b->data' is declared WEAVEC_SIZED_BY(cap) but is given 4 bytes where 'b->cap' says 8 [weavec::annotation-mismatch]
  b->cap = 8;
  // CHECK: rfc0012-sized-fields.c:12:30: note: 'b->data' is declared here
}

void shrink_count_first(struct buf *b) {
  b->cap = 8;
  // CHECK: rfc0012-sized-fields.c:[[@LINE+1]]:3: error: 'b->data' is declared WEAVEC_SIZED_BY(cap) but is given 4 bytes where 'b->cap' says 8 [weavec::annotation-mismatch]
  b->data = malloc(4);
}

// The right size, a larger object, an unknown one, or null: nothing.
void right_size(struct buf *b, size_t n) {
  b->data = malloc(n);
  b->cap = n;
}
void larger(struct buf *b) {
  b->data = malloc(16);
  b->cap = 8;
}
void unknown_size(struct buf *b, char *p) {
  b->data = p;
  b->cap = 8;
}
void cleared(struct buf *b) {
  b->data = NULL;
  b->cap = 8;
}

// -- Malformed annotations ----------------------------------------------------

struct bad {
  // CHECK: rfc0012-sized-fields.c:[[@LINE+1]]:30: warning: field 'p' is declared WEAVEC_SIZED_BY(nope) but 'nope' is not an integer field of 'struct bad' [weavec::invalid-annotation]
  int *WEAVEC_SIZED_BY(nope) p;
  int q;
};

int bad_one(struct bad *b) { return b->p[0]; }
int bad_two(struct bad *b) { return b->p[1]; }

// The flexible array member of RFC 0011's model is unchanged: the extent is
// the allocation's.
struct hack {
  size_t len;
  char data[];
};

void flexible(size_t n) {
  // RFC 0017: both n - 1 and the allocation's header addition must fit.
  if (n == 0 || n > (size_t)-1 - sizeof(struct hack))
    return;
  struct hack *h = malloc(sizeof *h + n);
  if (!h)
    return;
  h->data[n - 1] = 0;
  // CHECK: rfc0012-sized-fields.c:[[@LINE+1]]:3: error: 'h->data[n]' is out of bounds: it reaches 'n' + 9 bytes into 'h', which has 'n' + 8 bytes [weavec::out-of-bounds]
  h->data[n] = 0;
  free(h);
}

// -- Inference ----------------------------------------------------------------

struct vec {
  int *items;
  size_t n;
  size_t cap;
};

// `init` witnesses `(items, cap, 4)`; `push` writes `n` without storing
// `items`, refuting `(items, n)`; nothing refutes `(items, cap)`. The reader
// is analysed once more with the count in force (RFC 0012, *Two passes in a
// unit*): `last` is reported, `last_n` is not.
void init(struct vec *v, size_t n) {
  v->items = malloc(n * sizeof *v->items);
  v->cap = n;
  v->n = 0;
}

void push(struct vec *v, int x) {
  if (v->n < v->cap)
    v->items[v->n++] = x;
}

// CHECK: rfc0012-sized-fields.c:[[@LINE+1]]:34: error: 'v->items[v->cap]' is out of bounds: the access exceeds the allocation's converted size [weavec::out-of-bounds]
int last(struct vec *v) { return v->items[v->cap]; }
// CHECK: rfc0012-sized-fields.c:108:8: note: 'v->items' is declared here

int last_n(struct vec *v) { return v->items[v->n]; }
