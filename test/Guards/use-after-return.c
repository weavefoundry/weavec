// RFC 0037 §9: a frame's shadow says it returned, so a read through a
// pointer to a local of a function that returned traps until another frame
// reuses those bytes; a call of a function without tracked locals over the
// same bytes clears only what it reaches (the runtime finds it stale).
// RUN: %weavec_cc -O2 %s -o %t
// RUN: %t
// RUN: not --crash %t stale 2>&1 | FileCheck %s
#include <string.h>

void sink(int *);

__attribute__((noinline)) int *escape(void) {
  int local[4] = {1, 2, 3, 4};
  sink(local);
  return local;
}

int *kept;
__attribute__((noinline)) void sink(int *p) { kept = p; }

// A function with no tracked local: its frame does not write the shadow.
__attribute__((noinline)) int plain(int x) { return x * 3 + 1; }

int main(int argc, char **argv) {
  int *p = escape();
  (void)plain(argc);
  if (argc > 1 && strcmp(argv[1], "stale") == 0)
    // CHECK: weavec: stack-use-after-return at {{.*}}use-after-return.c:[[@LINE+2]]:{{[0-9]+}}: read of 4 bytes
    // CHECK: weavec: 0x{{[0-9a-f]+}} is in a frame that returned
    return p[1];
  return 0;
}
