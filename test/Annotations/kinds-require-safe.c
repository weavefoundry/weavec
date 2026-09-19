// RFC 0030 §6.3 and *Annotation surface*: WEAVEC_REQUIRE_SAFE before a
// function definition holds that function's sites to
// -fweavec-require=checked whatever the command line says: a facet that is
// neither proven nor checkable is an `unresolved-operation` error there, and
// only there. It replaces WEAVEC_CHECKED, which weavec.h no longer defines.
// On anything but a function it is an invalid annotation.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
// RUN: %weavec --dump-kinds %s -- -DDUMP 2>/dev/null | FileCheck --check-prefix=DUMP %s
#include <weavec.h>

#ifdef WEAVEC_CHECKED
#error "weavec.h 0.9 no longer defines WEAVEC_CHECKED"
#endif

char *get(void);

// DUMP: require-safe strict
WEAVEC_REQUIRE_SAFE int strict(void) {
  // CHECK: kinds-require-safe.c:[[@LINE+1]]:10: error: access 'get()[1000]' is neither proven nor checkable: {{.*}} [weavec::unresolved-operation]
  return get()[1000];
}

// The same access in an ordinary function is not an error.
// CHECK-NOT: kinds-require-safe.c:[[@LINE+2]]:{{.*}} error
// DUMP-NOT: require-safe lax
int lax(void) { return get()[1000]; }

#ifdef DUMP
// DUMP: problem [[@LINE+1]]:25: WEAVEC_REQUIRE_SAFE on 'flag', which is not a function
WEAVEC_REQUIRE_SAFE int flag;
#endif
