// RFC 0042 §4, §6: a typed function believes the bounds its loaded
// pointers declare and compares accesses with them (`pointer-out-of-bounds`,
// exact inside an object); each store of a pointer into a declared place is
// compared with its bound (`bounds-conversion`); a count that grows alone
// is checked against the shadow; a string converted from a buffer must have
// its zero byte (`unterminated-string`). A guarded function that breaks
// typed memory is trusted (T-A1), and reported under
// -fweavec-safe-checks=full.
// RUN: %weavec_cc -O2 -g -fweavec-typed %s -o %t
// RUN: %t
// RUN: not --crash %t index 2>&1 | FileCheck %s --check-prefix=INDEX
// RUN: not --crash %t inside 2>&1 | FileCheck %s --check-prefix=INSIDE
// RUN: not --crash %t pair 2>&1 | FileCheck %s --check-prefix=PAIR
// RUN: not --crash %t lone 2>&1 | FileCheck %s --check-prefix=LONE
// RUN: not --crash %t single 2>&1 | FileCheck %s --check-prefix=SINGLE
// RUN: not --crash %t grow 2>&1 | FileCheck %s --check-prefix=GROW
// RUN: not --crash %t string 2>&1 | FileCheck %s --check-prefix=STRING
// RUN: not --crash %t ornull 2>&1 | FileCheck %s --check-prefix=ORNULL
// RUN: not --crash %t two 2>&1 | FileCheck %s --check-prefix=TWO
// RUN: %t lie
// RUN: %weavec_cc -O2 -g -fweavec-typed -fweavec-safe-checks=full %s -o %t.full
// RUN: not --crash %t.full lie 2>&1 | FileCheck %s --check-prefix=FULL
#include <weavec.h>
#include <stdlib.h>
#include <string.h>

struct vec { int *WEAVEC_COUNTED_BY(cap) data; size_t cap; };
struct record { char name[8]; int id; };
struct holder { struct record *one; const char *WEAVEC_NULL_TERMINATED label; };
struct small { int x; };

int get(const struct vec *v, size_t i) {
  // INDEX: weavec: pointer-out-of-bounds at {{.*}}typed-bounds.c:[[@LINE+1]]:{{[0-9]+}}: read of 4 bytes
  return v->data[i];
}

char name(const struct holder *h, size_t i) {
  // An index past `name` stops at the field, inside the record's block.
  // INSIDE: weavec: index-out-of-bounds at {{.*}}typed-bounds.c:[[@LINE+1]]:{{[0-9]+}}: index 8
  return h->one->name[i];
}

void replace(struct vec *v, int *WEAVEC_ESCAPES WEAVEC_COUNTED_BY(n) d,
             size_t n) {
  v->data = d;
  // PAIR: weavec: bounds-conversion at {{.*}}typed-bounds.c:[[@LINE+1]]:{{[0-9]+}}: needs 20 bytes at 0x{{[0-9a-f]+}}
  v->cap = n + 1;
}

void swap(struct vec *v, int *WEAVEC_ESCAPES WEAVEC_COUNTED_BY(n) d,
          size_t n) {
  (void)n;
  // LONE: weavec: bounds-conversion at {{.*}}typed-bounds.c:[[@LINE+1]]:{{[0-9]+}}: needs 32 bytes at 0x{{[0-9a-f]+}}
  v->data = d;
}

void keep(struct holder *h, struct small *WEAVEC_ESCAPES s) {
  // SINGLE: weavec: bounds-conversion at {{.*}}typed-bounds.c:[[@LINE+1]]:{{[0-9]+}}: needs 12 bytes at 0x{{[0-9a-f]+}}
  h->one = (struct record *)s;
}

void stretch(struct vec *v) {
  // GROW: weavec: heap-buffer-overflow at {{.*}}typed-bounds.c:[[@LINE+1]]:{{[0-9]+}}
  v->cap += 4;
}

void label(struct holder *h,
           const char *WEAVEC_ESCAPES WEAVEC_COUNTED_BY(n) text, size_t n) {
  // STRING: weavec: unterminated-string at {{.*}}typed-bounds.c:[[@LINE+1]]:{{[0-9]+}}
  h->label = text;
}

size_t length(const struct holder *h) {
  size_t n = 0;
  for (const char *s = h->label; *s; s++)
    n++;
  return n;
}

// A null `_OR_NULL` pointer has no elements, whatever its count says.
int element(const int *WEAVEC_COUNTED_BY_OR_NULL(n) v, size_t n, size_t i) {
  // ORNULL: weavec: pointer-out-of-bounds at {{.*}}typed-bounds.c:[[@LINE+1]]:{{[0-9]+}}: read of 4 bytes
  return v[i];
}

// A count that bounds two pointers, grown alone: each pointer's object must
// have the bytes.
struct both { int *WEAVEC_COUNTED_BY(n) a; int *WEAVEC_COUNTED_BY(n) b; size_t n; };
void widen(struct both *s) {
  // TWO: weavec: heap-buffer-overflow at {{.*}}typed-bounds.c:[[@LINE+1]]:{{[0-9]+}}
  s->n += 4;
}

// Code that is not typed is trusted to keep the bounds it stores.
WEAVEC_GUARDED void lie(struct vec *v) { v->cap = 12; }

WEAVEC_GUARDED int main(int argc, char **argv) {
  struct vec v = {calloc(8, sizeof(int)), 8};
  struct record r = {"abc", 1};
  struct holder h = {&r, "label"};
  // On the heap: an escaping parameter may not point into a stack.
  struct small *s = calloc(1, sizeof *s);
  int *four = calloc(4, sizeof *four);
  char *text = malloc(3);
  memcpy(text, "abc", 3);
  const char *mode = argc > 1 ? argv[1] : "";
  if (!strcmp(mode, "index"))
    return get(&v, 8);
  if (!strcmp(mode, "inside"))
    return name(&h, 8);
  if (!strcmp(mode, "pair"))
    replace(&v, four, 4);
  if (!strcmp(mode, "lone"))
    swap(&v, four, 4);
  if (!strcmp(mode, "single"))
    keep(&h, s);
  if (!strcmp(mode, "grow"))
    stretch(&v);
  if (!strcmp(mode, "string"))
    label(&h, text, 3);
  if (!strcmp(mode, "ornull"))
    return element(NULL, (size_t)1 << 20, 5000);
  if (!strcmp(mode, "two")) {
    struct both s = {calloc(16, sizeof(int)), calloc(4, sizeof(int)), 4};
    widen(&s);
  }
  if (!strcmp(mode, "lie")) {
    lie(&v);
    // Believed: the read lands past the block, unreported. Under full
    // checks the guard reports it.
    // FULL: weavec: heap-buffer-overflow at {{.*}}typed-bounds.c:{{[0-9]+}}
    (void)get(&v, 10);
    return 0;
  }
  return get(&v, 7) == 0 && name(&h, 2) == 'c' && length(&h) == 5 ? 0 : 1;
}
