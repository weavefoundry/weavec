// RFC 0030 §7.2: every source of a declared kind reaches the engine (§15
// item 14), one function per row of the table. A declared kind holds inside
// the definition under A1 and is checked at every call; a definite shortfall
// against an exact extent is the call's `out-of-bounds` error.
// RUN: not %weavec --ledger=%t.json %s -- 2>&1 | FileCheck %s
// RUN: python3 %S/Inputs/ledger-rows.py %t.json | FileCheck --check-prefix=ROWS %s
#include <stddef.h>
#include <weavec.h>

void *memset(void *, int, size_t);

// WEAVEC_COUNTED_BY on a parameter: `p[i]` for `i < n` is proven inside.
int counted(const int *WEAVEC_COUNTED_BY(n) p, size_t n) {
  int s = 0;
  for (size_t i = 0; i < n; i++)
    s += p[i]; // ROWS: counted:[[@LINE]] p[i] spatial=proven
  return s;
}

// A loop to `m` needs `counted(m)`, which no call checks: the declared
// `counted(n)` rules `p`, so `p[i]` is the body's check, never proven.
int counted_to(const int *WEAVEC_COUNTED_BY(n) p, size_t n, size_t m) {
  int s = 0;
  for (size_t i = 0; i < m; i++)
    s += p[i]; // ROWS: counted_to:[[@LINE]] p[i] spatial=checked:index
  return s;
}

// WEAVEC_SIZED_BY on a `void *` parameter counts bytes.
void sized(void *WEAVEC_SIZED_BY(n) dst, size_t n) {
  memset(dst, 0, n); // ROWS: sized:[[@LINE]] memset(dst,0,n) spatial=proven
}

// WEAVEC_ENDED_BY: `[p, end)` lies in one object (the engine has no extent
// term for a pointer difference, so the body's `*p` is unresolved).
int ended(const int *WEAVEC_ENDED_BY(end) p, const int *end) {
  int s = 0;
  for (; p < end; p++)
    s += *p;
  return s;
}

// WEAVEC_STRING: the scan stays inside the string.
size_t string(const char *WEAVEC_STRING s) {
  size_t n = 0;
  while (s[n]) // ROWS: string:[[@LINE]] s[n] spatial=proven
    n++;
  return n;
}

// WEAVEC_NONNULL: the dereference inside is proven.
int nonnull(const int *WEAVEC_NONNULL p) {
  return *p; // ROWS: nonnull:[[@LINE]] *p null=proven
}

// `counted_by` on a field: the index is checked against the count, whose
// read checks the object pointer itself (§10.3 rule 5).
struct pkt {
  int len;
  char *buf __attribute__((counted_by(len)));
};
char field(const struct pkt *p, int i) {
  return p->buf[i]; // ROWS: field:[[@LINE]] p->buf[i] spatial=checked:index
}

// `alloc_size`: the result is Sized(n) bytes, a declared kind.
void *grab(size_t n) __attribute__((alloc_size(1)));
char result(size_t i) {
  char *p = grab(8);
  return p ? p[i] : 0; // ROWS: result:[[@LINE]] p[i] spatial=checked:index
}

// `nonnull(1)` and `_Nonnull`: nullability at level 2.
int attr(const int *p) __attribute__((nonnull(1)));
int attr(const int *p) {
  return *p; // ROWS: attr:[[@LINE]] *p null=proven
}
int attr2(const int *_Nonnull p) {
  return *p; // ROWS: attr2:[[@LINE]] *p null=proven
}

// `T p[static N]`: Counted(N) and non-null.
int sum4(const int a[static 4]) {
  return a[3]; // ROWS: sum4:[[@LINE]] a[3] null=proven
  // ROWS: sum4:[[@LINE-1]] a[3] spatial=proven
}

// A VLA parameter `T p[n]`: Counted(n), nullable.
int vla(size_t n, const int p[n]) {
  int s = 0;
  for (size_t i = 0; i < n; i++)
    s += p[i]; // ROWS: vla:[[@LINE]] p[i] spatial=proven
  return s;
}

// `ownership_returns` and `ownership_takes`: an unknown callee's contract.
void *pool_get(size_t n) __attribute__((ownership_returns(pool)));
void pool_put(void *p) __attribute__((ownership_takes(pool, 1)));

void calls(size_t n) {
  int four[4] = {1, 2, 3, 4};
  int three[3] = {1, 2, 3};
  char b[4] = "abc";
  (void)counted(four, n); // ROWS: calls:[[@LINE]] counted(four,n) spatial=checked:len
  sized(b, n); // ROWS: calls:[[@LINE]] sized(b,n) spatial=checked:len
  (void)ended(four, four + 4); // ROWS: calls:[[@LINE]] ended(four,four+4) spatial=proven
  // CHECK: rfc0030-declared-kinds.c:[[@LINE+1]]:15: error: 'ended' requires 20 bytes behind 'four', which has 16 bytes [weavec::out-of-bounds]
  (void)ended(four, four + 5);
  (void)string(b); // ROWS: calls:[[@LINE]] string(b) spatial=checked:len
  (void)nonnull(n ? four : 0); // ROWS: calls:[[@LINE]] nonnull(n?four:0) null=checked:nonnull
  // CHECK: rfc0030-declared-kinds.c:[[@LINE+1]]:14: error: 'sum4' requires 16 bytes behind 'three', which has 12 bytes [weavec::out-of-bounds]
  (void)sum4(three);
  (void)vla(n, four); // ROWS: calls:[[@LINE]] vla(n,four) spatial=checked:len
  char *p = pool_get(8);
  if (!p)
    return;
  pool_put(p);
  // CHECK: rfc0030-declared-kinds.c:[[@LINE+1]]:3: error: use of 'p' after it was freed [weavec::use-after-free]
  p[0] = 1;
}
