// RFC 0035 §2.6: a function its author keeps from AddressSanitizer reads
// memory no guard should check (a conservative collector's scan of the
// stack): its accesses are unguarded, as in a WEAVEC_UNSAFE function.
// RUN: %weavec_cc -O1 %s -o %t
// RUN: %t | FileCheck %s
#include <stdio.h>

__attribute__((no_sanitize("address"))) static int scan(const char *p) {
  int s = 0;
  for (int i = -8; i < 24; i++)
    s += p[i];
  return s;
}

__attribute__((no_sanitize_address)) static int scan2(const char *p) {
  return p[-1] + p[16];
}

int main(void) {
  char buf[16] = {1};
  // CHECK: scanned
  printf("scanned %d\n", (scan(buf) + scan2(buf)) > -1000);
  return 0;
}
