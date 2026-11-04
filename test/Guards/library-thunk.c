// RFC 0037 §5: a library function whose address the unit takes, in code
// or in a global's initializer, is replaced there by a thunk whose call is
// guarded, so a call through the pointer is checked.
// RUN: %weavec_cc -O2 -S -emit-llvm %s -o - | FileCheck %s --check-prefix=IR
// RUN: %weavec_cc -O2 -fweavec-checks=report -c %s -o %t.o
// RUN: %weavec_cc -O2 %s -o %t
// RUN: %t 8
// RUN: not --crash %t 9 2>&1 | FileCheck %s
#include <stdlib.h>
#include <string.h>

struct functions {
  void *(*copy)(void *, const void *, size_t);
  char *(*duplicate)(const char *);
};

// IR: @table = {{.*}}{ ptr @__weavec_thunk_memcpy, ptr @__weavec_thunk_strdup }
// IR: define linkonce_odr ptr @__weavec_thunk_memcpy(
struct functions table = {memcpy, strdup};

int main(int argc, char **argv) {
  char *d = malloc(8);
  char *s = calloc(1, 16);
  // CHECK: weavec: heap-buffer-overflow at <unknown>: write of 9 bytes
  table.copy(d, s, (size_t)atoi(argv[argc - 1]));
  free(table.duplicate("x"));
  return d[0];
}
