// RFC 0040 §5.3: a stack report names the local and its function, from the
// descriptor the frame stores in its left redzone, and lists the locals.
// RUN: %weavec_cc -O2 %s -o %t
// RUN: not --crash %t over 2>&1 | FileCheck %s --check-prefix=OVER
// RUN: not --crash %t scope 2>&1 | FileCheck %s --check-prefix=SCOPE
#include <string.h>

int *volatile sink;

__attribute__((noinline)) static int fill(int i) {
  int counts[4] = {0};
  int other[2] = {0};
  sink = counts;
  sink = other;
  return sink == other ? counts[0] + ((int *volatile)counts)[i] : 0;
}

__attribute__((noinline)) static int scoped(void) {
  int *p;
  {
    int inner = 5;
    p = &inner;
    sink = p;
  }
  return *sink;
}

int main(int argc, char **argv) {
  if (strcmp(argv[1], "over") == 0)
    return fill(argc + 2);
  return scoped();
}
// OVER: weavec: stack-buffer-overflow at {{.*}}report-stack.c:15:{{[0-9]+}}: read of 4 bytes
// OVER-NEXT: #0 0x{{[0-9a-f]+}} in fill {{.*}}report-stack.c:15
// OVER: is 0 bytes after 'counts' (16 bytes at offset {{[0-9]+}}) in the frame of fill (thread T0), whose locals are:
// OVER-DAG: 'counts'
// OVER-DAG: 'other'
// OVER: weavec: summary: stack-buffer-overflow

// SCOPE: weavec: stack-use-after-scope at {{.*}}report-stack.c:25:{{[0-9]+}}: read of 4 bytes
// SCOPE: is 0 bytes inside 'inner' (4 bytes at offset {{[0-9]+}}), whose scope has ended, in the frame of scoped
