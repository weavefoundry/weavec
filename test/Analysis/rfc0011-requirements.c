// RFC 0011, *Bounds checks* (library calls) and *Extents in summaries*: a
// libc call with a buffer and a length is checked against the buffer's
// extent; an access a function makes unconditionally past what its
// parameter's type promises is a requirement in its summary, checked at
// every call; requirements compose through wrappers.
// RUN: not %weavec %s -- -ferror-limit=0 2>&1 | FileCheck %s
// RUN: not %weavec --dump-analysis %s -- 2>/dev/null | FileCheck --check-prefix=DUMP %s
#include "../Inputs/prelude.h"
#include <weavec.h>

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
char *fgets(char *, int, void *);
struct outer { char *buf; int k; };

// -- Library buffer/length pairs ----------------------------------------------

void copy(void) {
  char src[16];
  char *dst = malloc(8);
  if (!dst)
    return;
  memcpy(dst, src, 8);
  // CHECK: rfc0011-requirements.c:[[@LINE+1]]:10: error: 'memcpy' accesses 16 bytes of 'dst', which has 8 bytes [weavec::out-of-bounds]
  memcpy(dst, src, 16);
  // CHECK: rfc0011-requirements.c:[[@LINE-6]]:15: note: 'dst' is allocated here
  free(dst);
}

// The source side is checked too.
void source_short(void) {
  char big[400];
  char small[200];
  // CHECK: rfc0011-requirements.c:[[@LINE+1]]:15: error: 'memcpy' accesses 400 bytes of 'small', which has 200 bytes [weavec::out-of-bounds]
  memcpy(big, small, 400);
  // CHECK: rfc0011-requirements.c:[[@LINE-3]]:8: note: 'small' is declared here
}

void read_line(void *f) {
  char line[32];
  if (fgets(line, 32, f))
    use(line);
  // CHECK: rfc0011-requirements.c:[[@LINE+1]]:13: error: 'fgets' accesses 64 bytes of 'line', which has 32 bytes [weavec::out-of-bounds]
  if (fgets(line, 64, f))
    use(line);
}

// A symbolic length against a symbolic extent, with a relation.
void clear(char *WEAVEC_SIZED_BY(n) p, size_t n, size_t m) {
  if (m <= n)
    memset(p, 0, m);
  if (m > n)
    memset(p, 0, m);
}

// -- Requirements inferred from a body ---------------------------------------

// Format 30 carries no `requires-extent` (RFC 0031 §6.1): the requirement of
// a must-access is the Call site's (RFC 0030 §7.5), and the summaries below
// pin the accesses themselves, as stores at a constant byte offset or over an
// element range (RFC 0031 §4.9, *Summaries*).
// A constant access past the pointee's size.
// DUMP-LABEL: function 'put7':
// DUMP: store param0->#7 := int [0, 0]
static void put7(char *b) { b[7] = 0; }

// A symbolic one, in the parameter that indexes it.
// DUMP-LABEL: function 'put_n':
// DUMP: store *param0[*] elements [param1, param1 plus 1) := int [0, 0]
static void put_n(char *b, size_t n) { b[n] = 0; }

// A loop below a parameter: the boundary is the requirement. The `int`
// counter's range is not exported (RFC 0031 §4.9, *Known limits*), so the
// store is to some elements.
// DUMP-LABEL: function 'fill':
// DUMP: store *param0[*] := int [0, 0] may
static void fill(int *b, int n) {
  for (int i = 0; i < n; i++)
    b[i] = 0;
}

// What the type promises is not a requirement.
// DUMP-LABEL: function 'first':
// DUMP: store param0->k := int [1, 1]
static void first(struct outer *o) { o->k = 1; }

// RFC 0017: requirements keep both class guards and typed ordering guards,
// and record the first byte accessed as well as the end of the access. An
// access under a test of an integer parameter is a possible store in
// format 30 (RFC 0031 §6.1).
// DUMP-LABEL: function 'on_zero':
// DUMP: store param0->#7 := int [0, 0] may
static void on_zero(char *b, int n) {
  if (n == 0)
    b[7] = 0;
}
// DUMP-LABEL: function 'guarded':
// DUMP: store param0->#4 := int [0, 0] may
static void guarded(char *b, int n) {
  if (n > 4)
    b[4] = 0;
}

// A library call on a parameter is a requirement in its length.
// DUMP-LABEL: function 'clears':
// DUMP: store *param0 := unknown
static void clears(void *b, size_t n) { memset(b, 0, n); }

// A local index bounded above by a constant needs the boundary. RFC 0017
// represents a loop bounded by both a constant and a parameter with min.
// DUMP-LABEL: function 'put8':
// DUMP: store *param0[*] elements [0, 8) := int [0, 0]
static void put8(char *b) {
  for (int i = 0; i < 8; i++)
    b[i] = 0;
}
// DUMP-LABEL: function 'put_le8':
// DUMP: store *param0[*] elements [0, 9) := int [0, 0]
static void put_le8(char *b) {
  for (int i = 0; i <= 8; i++)
    b[i] = 0;
}
// DUMP-LABEL: function 'either':
// DUMP: store *param0[*] := int [0, 0] may
static void either(int *b, int n) {
  for (int i = 0; i < n && i < 16; i++)
    b[i] = 0;
}

// -- Checked at the call -----------------------------------------------------

void calls(void) {
  char small[4];
  char big[8];
  put7(big);
  // CHECK: rfc0011-requirements.c:[[@LINE+1]]:8: error: 'put7' requires 8 bytes behind 'small', which has 4 bytes [weavec::out-of-bounds]
  put7(small);
  put8(big);
  // CHECK: rfc0011-requirements.c:[[@LINE+1]]:8: error: 'put8' requires 8 bytes behind 'small', which has 4 bytes [weavec::out-of-bounds]
  put8(small);
  // CHECK: rfc0011-requirements.c:[[@LINE+1]]:11: error: 'put_le8' requires 9 bytes behind 'big', which has 8 bytes [weavec::out-of-bounds]
  put_le8(big);
  int four[4];
  either(four, 4);
  // RFC 0030 §7.5: `i < n && i < 16` is no canonical counted loop, so no
  // requirement comes from it; but the context of `either(four, 100)`
  // writes `b[0..16)` on every return, past `four` (RFC 0031
  // *Implementation amendments*, "Stores past the caller's object").
  // CHECK: rfc0011-requirements.c:[[@LINE+1]]:10: error: 'either' requires 64 bytes behind 'four', which has 16 bytes [weavec::out-of-bounds]
  either(four, 100);
  char *heap = malloc(4);
  if (!heap)
    return;
  put_n(heap, 3);
  // CHECK: rfc0011-requirements.c:[[@LINE+1]]:9: error: 'put_n' requires 5 bytes behind 'heap', which has 4 bytes [weavec::out-of-bounds]
  put_n(heap, 4);
  int ints[4];
  fill(ints, 4);
  // CHECK: rfc0011-requirements.c:[[@LINE+1]]:8: error: 'fill' requires 32 bytes behind 'ints', which has 16 bytes [weavec::out-of-bounds]
  fill(ints, 8);
  // Clean: a call that needs nothing (Lua's `tablerehash(tb->hash, 0, n)`)
  // is satisfied by any object; it is not an access before the start.
  fill(ints, 0);
  clears(small, 0);
  on_zero(small, 1);
  // RFC 0030 §7.5: an access under a branch is no must-access, and a
  // `LibrarySpec` byte requirement is none of R1-R5: neither call is checked
  // against a requirement.
  // But the context of `on_zero(small, 0)` takes the branch and stores to
  // `b[7]` on every return (RFC 0031 *Implementation amendments*, "Stores
  // past the caller's object").
  // CHECK: rfc0011-requirements.c:[[@LINE+1]]:11: error: 'on_zero' requires 8 bytes behind 'small', which has 4 bytes [weavec::out-of-bounds]
  on_zero(small, 0);
  clears(small, 4);
  // And the context of `clears(small, 5)` has `memset` write five bytes.
  // CHECK: rfc0011-requirements.c:[[@LINE+1]]:10: error: 'clears' requires 5 bytes behind 'small', which has 4 bytes [weavec::out-of-bounds]
  clears(small, 5);
  free(heap);
}

// The argument's own position in its object counts.
void at_offset(void) {
  char buf[10];
  put7(buf + 2);
  // CHECK: rfc0011-requirements.c:[[@LINE+1]]:8: error: 'put7' requires 11 bytes behind 'buf', which has 10 bytes [weavec::out-of-bounds]
  put7(&buf[3]);
}

// -- Through wrappers --------------------------------------------------------

// The extent of a wrapped allocation reaches the caller.
// DUMP-LABEL: function 'xmalloc':
// DUMP: result fresh#0 free extent param0 zeroed when nonnull
static char *xmalloc(size_t n) {
  char *p = malloc(n);
  if (!p)
    __builtin_trap();
  return p;
}

// A callee's requirement on what this function passes through is this
// function's requirement.
// DUMP-LABEL: function 'deeper':
// DUMP: store param0->#7 := int [0, 0]
static void deeper(char *b) { put7(b); }

void via_wrappers(void) {
  char *p = xmalloc(4);
  // CHECK: rfc0011-requirements.c:[[@LINE+1]]:3: error: 'p[4]' is out of bounds: index 4 of an object of 4 bytes [weavec::out-of-bounds]
  p[4] = 0;
  // RFC 0030 §7.5: a callee's requirement does not compose into its caller's
  // (R1-R5 name accesses and string library calls only); but `deeper`
  // stores to `b[7]` on every return, past `p` (RFC 0031 *Implementation
  // amendments*, "Stores past the caller's object").
  // CHECK: rfc0011-requirements.c:[[@LINE+1]]:10: error: 'deeper' requires 8 bytes behind 'p', which has 4 bytes [weavec::out-of-bounds]
  deeper(p);
  free(p);
}
