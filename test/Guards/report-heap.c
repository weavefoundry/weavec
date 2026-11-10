// RFC 0040 §5.1, §5.2: a heap report prints the access's stack, the block's
// release and allocation stacks with files and lines (line tables by
// default, no -g), and a summary line; without recording, it says so.
// RUN: %weavec_cc -O2 %s -o %t
// RUN: not --crash %t free 2>&1 | FileCheck %s --check-prefix=FREED
// RUN: not --crash %t over 2>&1 | FileCheck %s --check-prefix=OVER
// RUN: env WEAVEC_RT_HEAP_STACKS=0 not --crash %t free 2>&1 \
// RUN:   | FileCheck %s --check-prefix=OFF
#include <stdlib.h>
#include <string.h>

char *volatile sink;

__attribute__((noinline)) static char *make(void) {
  char *p = malloc(32);
  sink = p;
  return p;
}
__attribute__((noinline)) static void drop(char *p) {
  free(p);
  sink = 0;
}
__attribute__((noinline)) static int use(char *p, int i) {
  int v = p[i];
  sink = p;
  return v;
}

int main(int argc, char **argv) {
  char *p = make();
  if (argc > 1 && strcmp(argv[1], "free") == 0)
    drop(p);
  int v = use(p, argc > 1 && strcmp(argv[1], "over") == 0 ? 32 : 0);
  sink = 0;
  return v;
}
// FREED: weavec: heap-use-after-free at {{.*}}report-heap.c:24:{{[0-9]+}}: read of 1 bytes at 0x[[P:[0-9a-f]+]]
// FREED-NEXT: #0 0x{{[0-9a-f]+}} in use {{.*}}report-heap.c:24
// FREED-NEXT: #1 0x{{[0-9a-f]+}} in main {{.*}}report-heap.c:33
// FREED: weavec: 0x[[P]] is 0 bytes inside the 32-byte heap block at 0x[[P]], released by thread T0:
// FREED-NEXT: #0 0x{{[0-9a-f]+}} in drop {{.*}}report-heap.c:20
// FREED-NEXT: #1 0x{{[0-9a-f]+}} in main {{.*}}report-heap.c:32
// FREED: weavec: the block was allocated by thread T0:
// FREED-NEXT: #0 0x{{[0-9a-f]+}} in make {{.*}}report-heap.c:15
// FREED-NEXT: #1 0x{{[0-9a-f]+}} in main {{.*}}report-heap.c:30
// FREED: weavec: summary: heap-use-after-free at {{.*}}report-heap.c:24:{{[0-9]+}} in use (thread T0)

// OVER: weavec: heap-buffer-overflow at {{.*}}report-heap.c:24:{{[0-9]+}}: read of 1 bytes
// OVER: is 0 bytes after the 32-byte heap object at 0x{{[0-9a-f]+}}, allocated by thread T0:
// OVER-NEXT: #0 0x{{[0-9a-f]+}} in make {{.*}}report-heap.c:15

// OFF: is 0 bytes inside the 32-byte heap block at 0x{{[0-9a-f]+}}; it was released (not recorded: WEAVEC_RT_HEAP_STACKS=0)
// OFF-NEXT: weavec: the block was allocated (not recorded: WEAVEC_RT_HEAP_STACKS=0)
