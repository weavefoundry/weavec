// RFC 0048 §3, §6.4, in Safe C: in a fast unit a string stepped over bytes
// a call compared is checked where it is stepped, byte by byte up to its
// terminator, and its reads then have no guard; a string handed in or
// returned is not searched again; a read-only library call on strings the
// function holds calls the C library itself.
// RUN: %weavec_cc -O2 -fweavec-safe -S -emit-llvm %s -o - | FileCheck %s
// RUN: %weavec_cc -O2 -fweavec-safe %s -o %t && %t
// RUN: not --crash %t short 2>&1 | FileCheck %s --check-prefix=SHORT
#include <string.h>
#include <weavec.h>

// CHECK-LABEL: define {{.*}}@skip.weavec.safe(
// CHECK: call {{.*}}@strncmp(
// CHECK: icmp eq i8 {{.*}}, 0
// CHECK: __weavec_rt_bounds_fail
// CHECK-NOT: __weavec_rt_guard
// CHECK-NOT: __weavec_rt_strlen
// CHECK: {{^}$}}
const char *WEAVEC_NULL_TERMINATED skip(const char *WEAVEC_NULL_TERMINATED p WEAVEC_LIFETIMEBOUND,
                                       const char *WEAVEC_NULL_TERMINATED head,
                                       size_t n) {
  if (strncmp(p, head, n) == 0)
    p += 4;
  return p;
}

// SHORT: weavec: pointer-out-of-bounds at {{.*}}ir-strings.c:[[@LINE-4]]
WEAVEC_GUARDED int main(int argc, char **argv) {
  static char text[] = "<!-- x";
  static char brief[] = "<!";
  if (argc > 1)
    return skip(brief, brief, 2)[0];
  return skip(text, "<!--", 4)[0] == ' ' ? 0 : 1;
}
