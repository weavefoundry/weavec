// RFC 0012, *Offset relations and lower bounds* and *Bounds checks through
// offsets*: `i <= n - 1` and `j = i + 1` are relations with an offset, `i >=
// 8` is a lower bound, and both decide bounds checks.
// RUN: not %weavec %s -- -ferror-limit=0 2>&1 | FileCheck %s
// RUN: not %weavec --dump-analysis %s -- 2>/dev/null | FileCheck --check-prefix=DUMP %s
#include "../Inputs/prelude.h"

// -- Offsets ------------------------------------------------------------------

void offsets(size_t n, size_t i) {
  int *a = malloc(n * sizeof *a);
  if (!a)
    return;
  if (i <= n - 1)
    a[i] = 0;
  if (i <= n - 1)
    // CHECK: rfc0012-relations.c:[[@LINE+1]]:5: error: 'a[i + 1]' may be out of bounds: 'i' may reach one below 'n', and 'a' has 'n' * 4 bytes [weavec::out-of-bounds]
    a[i + 1] = 0;
  if (i < n - 1)
    a[i + 1] = 0;
  free(a);
}

// A copy with an offset: `j == i + 1`.
void copies(size_t n, size_t i) {
  int *a = malloc(n * sizeof *a);
  if (!a)
    return;
  size_t j = i + 1;
  if (i < n)
    // CHECK: rfc0012-relations.c:[[@LINE+1]]:5: error: 'a[j]' may be out of bounds: 'j' may equal 'n', the number of elements of 'a' [weavec::out-of-bounds]
    a[j] = 0;
  if (j < n)
    a[j] = 0;
  free(a);
}

// -- Lower bounds -------------------------------------------------------------

// DUMP-LABEL: function 'lower':
void lower(size_t i) {
  char buf[8];
  if (i >= 8)
    // CHECK: rfc0012-relations.c:[[@LINE+1]]:5: error: 'buf[i]' is out of bounds: 'i' is at least 8 in an object of 8 bytes [weavec::out-of-bounds]
    buf[i] = 0;
  if (i > 7)
    // CHECK: rfc0012-relations.c:[[@LINE+1]]:5: error: 'buf[i]' is out of bounds: 'i' is at least 8 in an object of 8 bytes [weavec::out-of-bounds]
    buf[i] = 0;
  if (i >= 7)
    buf[i] = 0;
  if (i < 8)
    buf[i] = 0;
  // The failing edge of `i < 8` is `i >= 8`.
  if (i < 8)
    return;
  // CHECK: rfc0012-relations.c:[[@LINE+1]]:3: error: 'buf[i]' is out of bounds: 'i' is at least 8 in an object of 8 bytes [weavec::out-of-bounds]
  buf[i] = 0;
}

// The bound is in the state: `i >= 8` on the edge where `i < 8` fails (the
// other path never returns, so the exit state is that edge's).
// DUMP-LABEL: function 'bound':
// DUMP: relations{i >= 8}
void bound(size_t i) {
  if (i < 8)
    __builtin_trap();
}
