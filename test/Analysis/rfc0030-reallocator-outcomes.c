// RFC 0030 §8.2 and §9.1: an allocator that answers a zero new size by
// freeing its block and returning null (the C17 `realloc(p, 0)` contract) is
// a consume keyed on that outcome — `--dump-analysis` prints
// `outcome null{block: freed(free) when[nbytes =0]}`. Everything downstream
// turns on whether that `when` survives into the caller's summary.
//
// The three cases below are the reduction of what was the largest
// possible-temporal family on the corpus, and each pins a different half of
// the mechanism. They differ by one line at a time, so keep all three.
// RUN: %weavec %s -- 2>&1 | FileCheck %s
#include <stdlib.h>

struct table {
  void **slots;
  int size;
};

static void *reallocate(void *block, size_t nbytes) {
  if (nbytes == 0) {
    free(block);
    return NULL;
  }
  return realloc(block, nbytes);
}

// A. TRUE POSITIVE, and the one expectation here that must never be
// weakened. An unguarded retry really does free twice: for `nbytes == 0` the
// first call frees `block` and returns null, and the retry frees it again.
// CHECK: rfc0030-reallocator-outcomes.c:[[@LINE+5]]:13: warning: 'block' may be freed twice [weavec::double-free]
// CHECK: rfc0030-reallocator-outcomes.c:[[@LINE+2]]:17: note: previously freed here on some paths
void *retry_unguarded(void *block, size_t nbytes) {
  void *grown = reallocate(block, nbytes);
  if (grown == NULL)
    grown = reallocate(block, nbytes);
  return grown;
}

// B. CLEAN. Without a retry the `when[nbytes = 0]` guard reaches the caller,
// so `resize_plain`'s summary keeps `replaced` and two resizes in a row are
// not a double free — even though nothing here bounds the new size.
static void *call_once(void *block, size_t nbytes) {
  return reallocate(block, nbytes);
}

static void resize_plain(struct table *t, int n) {
  void **grown = (void **)call_once(t->slots, (size_t)n * 8);
  if (grown == NULL)
    return; /* leave the table as it was */
  t->slots = grown;
  t->size = n;
}

void two_plain_resizes(struct table *t) {
  resize_plain(t, t->size / 2);
  resize_plain(t, t->size / 2);
}

// C. CLEAN, and the reason the family above collapsed. The only difference
// from B is the guarded retry, which cannot free: it runs under
// `nbytes > 0`, the one condition under which `reallocate` does not free.
// The retry makes the returned value a merge of two call results, so the
// `PendingOutcome` of each reaches the merge at the end of the `if`. The
// retry's null class consumes nothing — `notePendingOutcome` drops a consume
// whose guard the arguments refute, and `nbytes > 0` refutes
// `when[nbytes = 0]` — and `PendingOutcome::unite` (lib/Core/AnalysisState.cpp,
// RFC 0030 §9.1) used to read that silence as disagreement and erase the
// outcome altogether. `call_with_retry` then had no classes, `resize_retry`
// lost `replaced`, and the caller saw two unguarded frees.
//
// A class one side consumes nothing on now leaves the other side's guarded
// consume standing, so `call_with_retry`'s summary matches
// `call_once`'s. Keep A warning: it is the same shape without the guard,
// and it is a real double free.
static void *call_with_retry(void *block, size_t nbytes) {
  void *grown = reallocate(block, nbytes);
  if (grown == NULL && nbytes > 0)
    grown = reallocate(block, nbytes); /* after an emergency collection */
  return grown;
}

static void resize_retry(struct table *t, int n) {
  if (n <= 0)
    return; /* the new size is non-zero, so nothing below can free */
  {
    void **grown = (void **)call_with_retry(t->slots, (size_t)n * 8);
    if (grown == NULL)
      return;
    t->slots = grown;
    t->size = n;
  }
}

void two_retry_resizes(struct table *t) {
  resize_retry(t, t->size / 2);
  resize_retry(t, t->size / 2);
}

// A's is the only one: neither B nor C may add another.
// CHECK-NOT: freed twice
