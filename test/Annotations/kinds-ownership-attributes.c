// RFC 0030 §7.2, row 8: `malloc`, `ownership_returns(m)`,
// `ownership_takes(m, i)` and `ownership_holds(m, i)` on a function state an
// ownership contract (a fresh result of family `m`; argument `i` released or
// retained), which the unknown-callee rules (§5.1) read. They are contracts,
// not pointer kinds: the result's kind stays the A3 default.
// RUN: %weavec --dump-kinds %s -- | FileCheck %s

typedef unsigned long size_t;

// CHECK: result arena_new: single nullable [default, lower-bound]
// CHECK-NEXT: ownership arena_new: fresh(free) [ecosystem]
void *arena_new(size_t n) __attribute__((malloc));
// CHECK: result pool_get: single nullable [default, lower-bound]
// CHECK-NEXT: ownership pool_get: fresh(pool) [ecosystem]
void *pool_get(size_t n) __attribute__((ownership_returns(pool)));
// CHECK: ownership pool_put: takes(0, pool) [ecosystem]
void pool_put(void *p) __attribute__((ownership_takes(pool, 1)));
// CHECK: ownership pool_keep: holds(1, pool) [ecosystem]
void pool_keep(int tag, void *p) __attribute__((ownership_holds(pool, 2)));

void use(void) {
  void *p = pool_get(8);
  pool_keep(1, p);
  pool_put(p);
  (void)arena_new(8);
}
