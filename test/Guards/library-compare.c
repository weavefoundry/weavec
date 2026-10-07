// RFC 0035 §2.5: the comparisons and searches run as the C library's, and
// check only the bytes they read: a case-insensitive comparison stays
// case-insensitive, and a bound larger than an object that the call stops
// short of is no error.
// RUN: %weavec_cc -O2 %s -o %t
// RUN: %t | FileCheck %s
// RUN: %weavec_cc -O0 %s -o %t0
// RUN: %t0 | FileCheck %s
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int main(int argc, char **argv) {
  const char *header = argc > 5 ? argv[1] : "Content-Length";
  size_t n = (size_t)argc + 13;
  char *four = malloc(4);
  memcpy(four, "abcd", 4);
  // CHECK: 0 0
  printf("%d %d\n", strncasecmp(header, "content-length", n),
         strcasecmp(header, "CONTENT-LENGTH"));
  // CHECK-NEXT: found b, differs
  printf("found %c, %s\n", *(char *)memchr(four, 'b', 64),
         strncmp(four, "HEAD /", 6) != 0 ? "differs" : "same");
  free(four);
  return 0;
}
