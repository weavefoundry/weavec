// RFC 0041 §4, §5: an access of a safe function is checked against its
// pointer's bounds (`pointer-out-of-bounds`), and a counted parameter's
// bytes against the shadow when the function is entered, so a guarded
// caller that overstates a count stops at the entry; a string's accesses
// keep their guards.
// RUN: %weavec_cc -O2 -g %s -o %t
// RUN: %t
// RUN: not --crash %t over 2>&1 | FileCheck %s --check-prefix=OVER
// RUN: not --crash %t heap 2>&1 | FileCheck %s --check-prefix=HEAP
// RUN: not --crash %t lie 2>&1 | FileCheck %s --check-prefix=LIE
// RUN: not --crash %t string 2>&1 | FileCheck %s --check-prefix=STRING
// RUN: %weavec_cc -O0 -g %s -o %t0
// RUN: not --crash %t0 over 2>&1 | FileCheck %s --check-prefix=OVER
#include <weavec.h>
#include <stdlib.h>
#include <string.h>

WEAVEC_SAFE int sum(const int *WEAVEC_COUNTED_BY(n) v, size_t n, size_t extra) {
  int s = 0;
  for (size_t i = 0; i < n + extra; i++)
    // OVER: weavec: pointer-out-of-bounds at {{.*}}safe-bounds.c:[[@LINE+1]]:{{[0-9]+}}: read of 4 bytes at 0x{{[0-9a-f]+}}
    s += v[i];
  return s;
}

WEAVEC_SAFE int heap(size_t n, size_t i) {
  int *a = malloc(n * sizeof *a);
  for (size_t k = 0; k < n; k++)
    a[k] = (int)k;
  // HEAP: weavec: pointer-out-of-bounds at {{.*}}safe-bounds.c:[[@LINE+1]]:{{[0-9]+}}: read of 4 bytes
  int r = a[i];
  free(a);
  return r;
}

// A string's accesses keep their guards (amendment 13).
WEAVEC_SAFE size_t length(const char *WEAVEC_NULL_TERMINATED s) {
  size_t i = 0;
  // STRING: weavec: heap-buffer-overflow at {{.*}}safe-bounds.c:[[@LINE+1]]
  while (s[i])
    i++;
  return i;
}

// LIE: weavec: stack-buffer-overflow at {{.*}}safe-bounds.c:[[@LINE+1]]
WEAVEC_SAFE int first(const int *WEAVEC_COUNTED_BY(n) v, size_t n) {
  return n ? v[0] : 0;
}

int main(int argc, char **argv) {
  int a[4] = {1, 2, 3, 4};
  if (argc < 2)
    return sum(a, 4, 0) == 10 && heap(8, 3) == 3 && length("abc") == 3 &&
                   first(a, 4) == 1
               ? 0
               : 1;
  if (!strcmp(argv[1], "over"))
    return sum(a, 4, 1);
  if (!strcmp(argv[1], "heap"))
    return heap(8, 8);
  if (!strcmp(argv[1], "lie"))
    return first(a, 8);
  char *unterminated = malloc(4);
  memcpy(unterminated, "abcd", 4);
  return (int)length(unterminated);
}
