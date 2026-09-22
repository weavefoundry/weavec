// RFC 0030 §9.4, *Owner uniqueness*: a release through a name that is not an
// owning place must not be re-attributed to the object that contains it. A
// field pointer aimed at an inner member of its own object (`s->cur =
// &s->first`) is such a name: releasing what `s->cur` points at is a release
// of `s->cur`, not of `s`, and a use of `s` afterwards is at most possible.
// The record such a release leaves on `s` is local — no summary may carry it
// — and a record of unknown origin joined into it must not lift that.
// RUN: %weavec %s -- 2>&1 | FileCheck --check-prefix=CLEAN --allow-empty %s
// RUN: %weavec --dump-analysis %s -- 2>/dev/null | FileCheck --check-prefix=DUMP %s
#include <stdlib.h>

struct link {
  struct link *next;
};

struct holder {
  struct link first;
  struct link *cur;
  int count;
};

extern void opaque(struct holder *h);

static void reset(struct holder *h) { h->cur = &h->first; }

// Releases the chain hanging off `h->cur`; `h->cur` itself is replaced.
static void drop(struct holder *h) {
  struct link *it = h->cur;
  while (it != NULL) {
    struct link *next = it->next;
    free(it);
    it = next;
    h->count--;
  }
  h->cur = NULL;
}

// The release is `h->cur`'s, not `h`'s: the summary must say so, and the
// unknown callee in between must not turn the local record on `h` into an
// exported one.
// DUMP-LABEL: function 'walk':
// DUMP: summary: {{.*}}h->count: {{[a-z|]*}}written{{.*}}h->cur: {{[a-z|]*}}freed(free){{[a-z|]*}}replaced
// DUMP-NOT: summary: h: {{[a-z|]*}}freed
void walk(struct holder *h) {
  reset(h);
  opaque(h);
  drop(h);
}

// And the caller of such a function keeps its own object: no use-after-free
// on `h` at the read, and none on the second call either.
// CLEAN-NOT: error:
// CLEAN-NOT: use-after-free
// CLEAN-NOT: double-free
int use(struct holder *h) {
  walk(h);
  walk(h);
  return h->count;
}
