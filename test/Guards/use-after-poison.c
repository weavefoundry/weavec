// RFC 0036 §5: weavec-cc builds code as ASan would (`__has_feature(
// address_sanitizer)`), and a pool written for ASan poisons its free space
// through ASan's interface: an access to a poisoned byte stops.
// RUN: %weavec_cc -O1 %s -o %t
// RUN: not --crash %t 2>&1 | FileCheck %s
// RUN: %weavec_cc -fno-weavec-asan-compat -O1 %s -o %t.plain
// RUN: %t.plain | FileCheck --check-prefix=PLAIN %s
#include <stdio.h>
#include <stdlib.h>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#include <sanitizer/asan_interface.h>
#define POISONS 1
#endif
#endif

int main(void) {
  char *pool = malloc(64);
  if (pool == NULL)
    return 1;
#ifdef POISONS
  ASAN_POISON_MEMORY_REGION(pool + 32, 32);
#endif
  pool[0] = 1;
  // PLAIN: 0
  // CHECK: weavec: use-after-poison at {{.*}}use-after-poison.c:[[@LINE+1]]:{{[0-9]+}}: read of 1 bytes at 0x{{[0-9a-f]+}}
  printf("%d\n", pool[40]);
  return 0;
}
