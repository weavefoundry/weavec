// RFC 0040 §5.5: a bad release has a kind of its own, the location of the
// guarded call that made it (or, through a function pointer, its stack's
// first frame), its stack, and the block's.
// RUN: %weavec_cc -O2 %s -o %t
// RUN: not --crash %t double 2>&1 | FileCheck %s --check-prefix=DOUBLE
// RUN: not --crash %t pointer 2>&1 | FileCheck %s --check-prefix=POINTER
// RUN: not --crash %t inside 2>&1 | FileCheck %s --check-prefix=INSIDE
// RUN: not --crash %t stack 2>&1 | FileCheck %s --check-prefix=STACK
#include <stdlib.h>
#include <string.h>

void (*volatile release)(void *) = free;
void *volatile sink;

__attribute__((noinline)) static char *make(void) {
  char *p = malloc(8);
  sink = p;
  return p;
}
__attribute__((noinline)) static void drop(char *p) { free(p); }

int main(int argc, char **argv) {
  char *p = make();
  sink = p;
  if (argc < 2)
    return 0;
  if (strcmp(argv[1], "double") == 0) {
    drop(p);
    drop(p);
  } else if (strcmp(argv[1], "pointer") == 0) {
    free(p);
    release(p);
  } else if (strcmp(argv[1], "inside") == 0) {
    free(p + 4);
  } else {
    char local[16];
    sink = local;
    free(sink);
  }
  return 0;
}
// DOUBLE: weavec: double-free at {{.*}}report-release.c:20:{{[0-9]+}}: free of 0x[[P:[0-9a-f]+]]
// DOUBLE-NEXT: #0 0x{{[0-9a-f]+}} in drop {{.*}}report-release.c:20
// DOUBLE-NEXT: #1 0x{{[0-9a-f]+}} in main {{.*}}report-release.c:29
// DOUBLE: weavec: 0x[[P]] is the 8-byte heap block at 0x[[P]], released by thread T0:
// DOUBLE-NEXT: #0 0x{{[0-9a-f]+}} in drop {{.*}}report-release.c:20
// DOUBLE-NEXT: #1 0x{{[0-9a-f]+}} in main {{.*}}report-release.c:28
// DOUBLE: weavec: the block was allocated by thread T0:
// DOUBLE-NEXT: #0 0x{{[0-9a-f]+}} in make {{.*}}report-release.c:16
// DOUBLE-NEXT: #1 0x{{[0-9a-f]+}} in main {{.*}}report-release.c:23
// DOUBLE: weavec: summary: double-free at {{.*}}report-release.c:20:{{[0-9]+}} in drop (thread T0)

// POINTER: weavec: double-free at {{.*}}report-release.c:32{{(:[0-9]+)?}}: free of 0x{{[0-9a-f]+}}
// POINTER-NEXT: #0 0x{{[0-9a-f]+}} in main {{.*}}report-release.c:32

// INSIDE: weavec: invalid-free at {{.*}}report-release.c:34:{{[0-9]+}}: free of 0x{{[0-9a-f]+}}: inside the 8-byte heap block at 0x{{[0-9a-f]+}}
// INSIDE: is 4 bytes inside the 8-byte heap object at 0x{{[0-9a-f]+}}, allocated by thread T0:
// INSIDE-NEXT: #0 0x{{[0-9a-f]+}} in make {{.*}}report-release.c:16

// STACK: weavec: invalid-free at {{.*}}report-release.c:38:{{[0-9]+}}: free of 0x{{[0-9a-f]+}}: not heap memory
