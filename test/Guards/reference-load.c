// RFC 0043 §7: a pointer a safe function loads from a field that is not an
// owner is a reference, tested where it is loaded and used: one into a
// released heap block stops with `heap-use-after-free`, in every check
// mode. A reference only compared or copied is not tested.
// RUN: %weavec_cc -O1 -g %s -o %t
// RUN: %t
// RUN: not --crash %t stale 2>&1 | FileCheck %s
// RUN: %weavec_cc -O1 -g -fweavec-checks=report %s -o %t.report
// RUN: %t.report stale 2>&1 | FileCheck %s --check-prefix=REPORT
// RUN: %weavec_cc -O1 -S -emit-llvm %s -o - | FileCheck %s --check-prefix=IR
#include <weavec.h>
#include <stdlib.h>
#include <string.h>

struct item { int v; };
struct cache { struct item *last; };

WEAVEC_SAFE __attribute__((noinline)) void remember(struct cache *c) {
  struct item *it = calloc(1, sizeof *it);
  if (it == NULL)
    return;
  it->v = 6;
  c->last = it;
  free(it);
}

// IR-LABEL: define {{.*}} @recall.weavec.safe(
// IR: icmp eq i8 {{.*}}, -3
// IR: call {{.*}}@__weavec_rt_reference_fail(
WEAVEC_SAFE __attribute__((noinline)) int recall(struct cache *c) {
  // CHECK: weavec: heap-use-after-free at {{.*}}reference-load.c:[[@LINE+3]]:{{[0-9]+}}: reference to 0x{{[0-9a-f]+}} loaded from a field
  // CHECK: is the 4-byte heap block at 0x{{[0-9a-f]+}}; it was released
  // REPORT: weavec: heap-use-after-free at {{.*}}reference-load.c:[[@LINE+1]]
  return c->last->v;
}

// IR-LABEL: define {{.*}} @same.weavec.safe(
// IR-NOT: __weavec_rt_reference_fail
// IR: ret
WEAVEC_SAFE __attribute__((noinline)) int same(struct cache *c,
                                               struct item *it) {
  return c->last == it;
}

int main(int argc, char **argv) {
  struct cache c = {0};
  struct item kept = {3};
  c.last = &kept;
  if (recall(&c) != 3 || !same(&c, &kept))
    return 1;
  if (argc > 1 && strcmp(argv[1], "stale") == 0) {
    remember(&c);
    return recall(&c) == 6 ? 0 : 0;
  }
  return 0;
}
