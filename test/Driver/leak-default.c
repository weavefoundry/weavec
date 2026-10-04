// RFC 0033 §8: a weavec-cc build prints no leak warnings unless asked;
// -Wweavec-leak or -Wweavec asks, and -Werror=weavec-leak makes them errors.
// The weavec tool, which only analyses, prints them.
//
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -c %s -o %t/a.o 2>&1 | count 0
// RUN: %weavec_cc -Wweavec-leak -c %s -o %t/b.o 2>&1 | FileCheck --check-prefix=LEAK %s
// RUN: %weavec_cc -Wweavec -c %s -o %t/c.o 2>&1 | FileCheck --check-prefix=LEAK %s
// RUN: not %weavec_cc -Werror=weavec-leak -c %s -o %t/d.o 2>&1 | FileCheck --check-prefix=ERROR %s
// RUN: %weavec %s -- 2>&1 | FileCheck --check-prefix=LEAK %s

#include <stdlib.h>

int keep(int n) {
  char *p = malloc((size_t)n + 1);
  if (p == NULL)
    return 0;
  p[0] = 1;
  return p[0];
}

// LEAK: warning: 'p' is leaked [weavec::leak]
// ERROR: error: 'p' is leaked [weavec::leak]
