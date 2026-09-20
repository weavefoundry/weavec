// RFC 0030 §7.2, row 6: `alloc_size(i[, j])` on a function gives its result
// `sized(arg i [* arg j])`, at the ecosystem level (level 4 in a system
// header). The result of a call with constant arguments is Single-valid
// when the size covers the pointee (§7.3), so storing it keeps a slot
// `single`.
// RUN: %weavec --dump-kinds %s -- | FileCheck %s

typedef unsigned long size_t;

// CHECK: result pool_alloc: sized(param 0 scale 1 plus 0) nullable [declared, declared, shape ecosystem]
void *pool_alloc(size_t n) __attribute__((alloc_size(1)));
// CHECK: result pool_calloc: sized(param 1 scale 1 plus 0) nullable [declared, declared, shape ecosystem] factor param 0
void *pool_calloc(size_t count, size_t size) __attribute__((alloc_size(2, 1)));

struct item { long a, b; };
struct list {
  // CHECK: field list.one: single nullable [inferred, lower-bound]
  struct item *one;
  // CHECK: field list.small: unknown nullable [inferred]
  // CHECK-NEXT: demoted at [[@LINE+8]]:3: a value with 8 bytes, fewer than the 16 of 'struct item'
  struct item *small;
  // CHECK: field list.many: single nullable [inferred, lower-bound]
  struct item *many;
};

void make(struct list *l) {
  l->one = pool_alloc(sizeof(struct item));
  l->small = pool_alloc(8);
  l->many = pool_calloc(4, sizeof(struct item));
}
