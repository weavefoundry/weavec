// RFC 0011, *Extents* and *Bounds checks*: an allocation or declaration
// gives an object its extent; an access at a constant or symbolic offset
// the extent cannot hold is `out-of-bounds`, with the index as written and
// the object's origin in a note.
// RUN: not %weavec %s -- -ferror-limit=0 2>&1 | FileCheck %s
// RUN: not %weavec --dump-analysis %s -- 2>&1 | FileCheck --check-prefix=DUMP %s
#include "../Inputs/prelude.h"
#include <weavec.h>

void *calloc(size_t, size_t);
void *memset(void *, int, size_t);

struct rec { char name[8]; int id; };
struct flex { int n; char data[]; };

// -- Constants ----------------------------------------------------------------

void declared(void) {
  char buf[10];
  buf[9] = 0;
  // CHECK: rfc0011-bounds.c:[[@LINE+1]]:3: error: 'buf[10]' is out of bounds: index 10 of an object of 10 bytes [weavec::out-of-bounds]
  buf[10] = 0;
  // CHECK: rfc0011-bounds.c:[[@LINE-4]]:8: note: 'buf' is declared here
}

// The extent of the allocation is in the summary of what returns it.
// DUMP-LABEL: function 'eight':
// DUMP: summary: stores{} returns{fresh(free) extent=8, null}
static char *eight(void) { return malloc(8); }

void heap(void) {
  char *p = eight();
  if (!p)
    return;
  p[7] = 0;
  // CHECK: rfc0011-bounds.c:[[@LINE+1]]:3: error: 'p[8]' is out of bounds: index 8 of an object of 8 bytes [weavec::out-of-bounds]
  p[8] = 0;
  // CHECK: rfc0011-bounds.c:[[@LINE-6]]:13: note: 'p' is allocated here
  free(p);
}

// A folded constant shows its value.
void folded(void) {
  int data = 10;
  int buffer[10];
  // CHECK: rfc0011-bounds.c:[[@LINE+1]]:3: error: 'buffer[data]' is out of bounds: index 'data' (10) of an object of 40 bytes [weavec::out-of-bounds]
  buffer[data] = 1;
}

void deref(void) {
  int *q = malloc(2 * sizeof *q);
  if (!q)
    return;
  *(q + 1) = 1;
  // CHECK: rfc0011-bounds.c:[[@LINE+1]]:3: error: '*(q + 2)' is out of bounds: index 2 of an object of 8 bytes [weavec::out-of-bounds]
  *(q + 2) = 1;
  free(q);
}

// A field past an allocation too small for its struct.
void short_struct(void) {
  struct rec *r = malloc(8);
  if (!r)
    return;
  r->name[0] = 0;
  // CHECK: rfc0011-bounds.c:[[@LINE+1]]:3: error: 'r->id' is out of bounds: it reaches 12 bytes into 'r', which has 8 bytes [weavec::out-of-bounds]
  r->id = 1;
  free(r);
}

// An array member is an object of its own inside its struct.
void member_array(struct rec *r) {
  r->name[7] = 0;
  // CHECK: rfc0011-bounds.c:[[@LINE+1]]:3: error: 'r->name[8]' is out of bounds: index 8 of an object of 8 bytes [weavec::out-of-bounds]
  r->name[8] = 0;
  // CHECK: rfc0011-bounds.c:13:19: note: 'r->name' is declared here
}

// -- Before the start ---------------------------------------------------------

void negative(void) {
  char buf[16];
  char *p = buf;
  // CHECK: rfc0011-bounds.c:[[@LINE+1]]:3: error: 'p[-1]' is out of bounds: index -1 is before the start of 'p' [weavec::out-of-bounds]
  p[-1] = 0;
  // CHECK: rfc0011-bounds.c:[[@LINE-4]]:8: note: the object behind 'p' is declared here
}

void underwrite(void) {
  char *buf = malloc(16);
  if (!buf)
    return;
  char *p = buf - 8;
  // CHECK: rfc0011-bounds.c:[[@LINE+1]]:3: error: 'p[0]' is out of bounds: index 0 is before the start of 'p' [weavec::out-of-bounds]
  p[0] = 'A';
  free(buf);
}

// -- The pointer's own position counts ----------------------------------------

void stepped(void) {
  int *q = malloc(8 * sizeof *q);
  if (!q)
    return;
  int *p = q + 2;
  p[5] = 0;
  // CHECK: rfc0011-bounds.c:[[@LINE+1]]:3: error: 'p[6]' is out of bounds: index 6 of an object of 32 bytes [weavec::out-of-bounds]
  p[6] = 0;
  free(q);
}

void walked(void) {
  char *s = malloc(4);
  if (!s)
    return;
  char *p = s;
  p++;
  p++;
  p++;
  *p = 0;
  p++;
  // CHECK: rfc0011-bounds.c:[[@LINE+1]]:3: error: '*p' is out of bounds: it reaches 5 bytes into 'p', which has 4 bytes [weavec::out-of-bounds]
  *p = 0;
  free(s);
}

// -- Symbolic extents ---------------------------------------------------------

// DUMP-LABEL: function 'ints':
// DUMP: summary: stores{} returns{fresh(free) extent=n*4, null}
static int *ints(int n) { return malloc(n * sizeof(int)); }
// DUMP-LABEL: function 'zeroed':
// DUMP: summary: stores{} returns{fresh(free) extent=n*4, null}
static int *zeroed(int n) { return calloc(n, sizeof(int)); }

void at_n(int n) {
  int *p = ints(n);
  if (!p)
    return;
  p[n - 1] = 0;
  // CHECK: rfc0011-bounds.c:[[@LINE+1]]:3: error: 'p[n]' is out of bounds: 'n' is the number of elements of 'p' [weavec::out-of-bounds]
  p[n] = 0;
  free(p);
}

void bytes(size_t n) {
  char *p = malloc(n + 1);
  if (!p)
    return;
  p[n] = 0;
  // CHECK: rfc0011-bounds.c:[[@LINE+1]]:3: error: 'p[n + 1]' is out of bounds: it reaches 'n' + 2 bytes into 'p', which has 'n' + 1 bytes [weavec::out-of-bounds]
  p[n + 1] = 0;
  free(p);
}

// A write to the counter forgets the extent expressed in it.
void counter_moves(int n) {
  int *p = zeroed(n);
  if (!p)
    return;
  n = n + 1;
  p[n - 1] = 0;
  free(p);
}

// The flexible-array idiom: `sizeof *f + n` bytes hold `data[0..n-1]`, and
// one more is the classic off-by-one.
void flexible(int n) {
  struct flex *f = malloc(sizeof *f + n);
  if (!f)
    return;
  f->data[n - 1] = 0;
  // CHECK: rfc0011-bounds.c:[[@LINE+1]]:3: error: 'f->data[n]' is out of bounds: it reaches 'n' + 5 bytes into 'f', which has 'n' + 4 bytes [weavec::out-of-bounds]
  f->data[n] = 0;
  free(f);
}

// -- Relations ----------------------------------------------------------------

void loops(int n) {
  int *p = ints(n);
  if (!p)
    return;
  for (int i = 0; i < n; i++)
    p[i] = 0;
  // CHECK: rfc0011-bounds.c:[[@LINE+2]]:5: error: 'p[i]' may be out of bounds: 'i' may equal 'n', the number of elements of 'p' [weavec::out-of-bounds]
  for (int i = 0; i <= n; i++)
    p[i] = 0;
  // CHECK: rfc0011-bounds.c:[[@LINE+2]]:5: error: 'p[i + 1]' may be out of bounds: 'i' may reach one below 'n', and 'p' has 'n' * 4 bytes [weavec::out-of-bounds]
  for (int i = 0; i < n; i++)
    p[i + 1] = 0;
  for (int i = 0; i < n - 1; i++)
    p[i + 1] = 0;
  free(p);
}

void guards(int i, int n) {
  int *p = ints(n);
  if (!p)
    return;
  if (i < n)
    p[i] = 1;
  // CHECK: rfc0011-bounds.c:[[@LINE+2]]:5: error: 'p[i]' is out of bounds: 'i' is at least 'n', the number of elements of 'p' [weavec::out-of-bounds]
  if (i >= n)
    p[i] = 1;
  // CHECK: rfc0011-bounds.c:[[@LINE+2]]:5: error: 'p[i]' is out of bounds: 'i' is above 'n', the number of elements of 'p' [weavec::out-of-bounds]
  if (i > n)
    p[i] = 1;
  // CHECK: rfc0011-bounds.c:[[@LINE+2]]:5: error: 'p[i]' is out of bounds: 'i' is at least 'n', the number of elements of 'p' [weavec::out-of-bounds]
  if (n <= i)
    p[i] = 1;
  free(p);
}

// A copy of the index carries the relation; a write to it drops it.
void copies(int i, int n) {
  int *p = ints(n);
  if (!p)
    return;
  int j = i;
  // CHECK: rfc0011-bounds.c:[[@LINE+2]]:5: error: 'p[i]' is out of bounds: 'i' is at least 'n', the number of elements of 'p' [weavec::out-of-bounds]
  if (j >= n)
    p[i] = 1;
  if (i >= n) {
    i = 0;
    p[i] = 1;
  }
  free(p);
}

// -- Library calls ------------------------------------------------------------

void clears(size_t n) {
  char *p = malloc(n);
  if (!p)
    return;
  memset(p, 0, n);
  // CHECK: rfc0011-bounds.c:[[@LINE+1]]:10: error: 'memset' accesses 'n' + 1 bytes of 'p', which has 'n' bytes [weavec::out-of-bounds]
  memset(p, 0, n + 1);
  free(p);
}

// -- Constant upper bounds ----------------------------------------------------

// `i < 8` bounds `i` above (RFC 0011, *Relations*): the loop's boundary may
// reach past a smaller object; a guard that fits proves the access.
void counted(void) {
  char buf[4];
  // CHECK: rfc0011-bounds.c:[[@LINE+2]]:5: error: 'buf[i]' may be out of bounds: 'i' may be 7 in an object of 4 bytes [weavec::out-of-bounds]
  for (int i = 0; i < 8; i++)
    buf[i] = 0;
  // CHECK: rfc0011-bounds.c:[[@LINE-4]]:8: note: 'buf' is declared here
  for (int i = 0; i < 4; i++)
    buf[i] = 0;
  // CHECK: rfc0011-bounds.c:[[@LINE+2]]:5: error: 'buf[i]' may be out of bounds: 'i' may be 4 in an object of 4 bytes [weavec::out-of-bounds]
  for (int i = 0; i <= 4; i++)
    buf[i] = 0;
}

void guarded(int i) {
  int ints[8];
  if (i >= 0 && i < 8)
    ints[i] = 0;
  // CHECK: rfc0011-bounds.c:[[@LINE+2]]:5: error: 'ints[i]' may be out of bounds: 'i' may be 8 in an object of 32 bytes [weavec::out-of-bounds]
  if (i >= 0 && i <= 8)
    ints[i] = 0;
}

// An object of at most `n <= 4` bytes cannot hold a constant access past 4.
void small_object(int n) {
  if (n > 4)
    return;
  char *p = malloc(n);
  if (!p)
    return;
  p[3] = 0;
  // CHECK: rfc0011-bounds.c:[[@LINE+1]]:3: error: 'p[4]' is out of bounds: index 4 of an object of 'n' bytes [weavec::out-of-bounds]
  p[4] = 0;
  free(p);
}

// A length bounded above by a constant, against a constant extent.
void bounded_length(size_t n) {
  char buf[4];
  if (n < 5)
    memset(buf, 0, n);
  // CHECK: rfc0011-bounds.c:[[@LINE+2]]:12: error: 'memset' may access past the end of 'buf': 'n' may be 8, and 'buf' has 4 bytes [weavec::out-of-bounds]
  if (n < 9)
    memset(buf, 0, n);
}
