// RFC 0038 §2: a search is checked after the call over the bytes it read,
// up to where its result says it stopped: a search inside an unterminated
// buffer that stops at a match there runs clean (git's fsck reads an
// object read into a block of exactly its size with strcspn), and one that
// reads past the buffer stops, naming the call: as an overflow of the bytes
// up to where it stopped, or, when it found nothing, as an unterminated
// string.
//
// RUN: %weavec_cc -O2 %s -o %t
// RUN: %t ok | FileCheck --check-prefix=OK %s
// RUN: not --crash %t strcspn 2>&1 | FileCheck --check-prefix=STRCSPN %s
// RUN: not --crash %t strchr 2>&1 | FileCheck --check-prefix=STRCHR %s
// RUN: not --crash %t memchr 2>&1 | FileCheck --check-prefix=MEMCHR %s
// RUN: not --crash %t strstr 2>&1 | FileCheck --check-prefix=STRSTR %s
// RUN: %weavec_cc -O0 %s -o %t0
// RUN: %t0 ok | FileCheck --check-prefix=OK %s
// RUN: not --crash %t0 strcspn 2>&1 | FileCheck --check-prefix=STRCSPN %s
//
// The guard after each call, over the result.
// RUN: %weavec_cc -O1 -S -emit-llvm %s -o - | FileCheck --check-prefix=IR %s
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* An object read into a block of exactly its size, with no terminator. */
static char *object(const char *text, size_t n) {
  char *p = malloc(n);
  memcpy(p, text, n);
  return p;
}

int main(int argc, char **argv) {
  char *tree = object("tree 1234\nparent", 16);
  char *nul = object("ab\0cd", 5);
  if (argc < 2)
    return 2;
  if (strcmp(argv[1], "ok") == 0) {
    // OK: 9 4 1 2 5 4
    printf("%zu %zu %zu %zu %zu %zu\n", strcspn(tree, "<>\n"),
           strspn(tree, "eert"), (size_t)(strchr(tree, 'r') - tree),
           (size_t)(strpbrk(tree, "e ") - tree),
           (size_t)(strstr(tree, "1234") - tree),
           (size_t)((char *)memchr(tree, ' ', 1000) - tree));
    // OK-NEXT: 2 cd
    printf("%zu %s\n", strcspn(nul, "x"), strchr(nul, 0) == nul + 2 ? "cd" : "?");
    return 0;
  }
  if (strcmp(argv[1], "strcspn") == 0)
    return (int)strcspn(tree, "<>");
  if (strcmp(argv[1], "strchr") == 0)
    return strchr(tree, 'z') != NULL;
  if (strcmp(argv[1], "memchr") == 0)
    return memchr(tree, 'z', 17) != NULL;
  return strstr(tree, "parents") != NULL;
}

// STRCSPN: weavec: heap-buffer-overflow at {{.*}}library-scan.c:[[@LINE-8]]:{{[0-9]+}}: read of {{[0-9]+}} bytes at
// STRCHR: weavec: unterminated-string at {{.*}}library-scan.c:[[@LINE-7]]:{{[0-9]+}}: read of {{[0-9]+}} bytes at
// MEMCHR: weavec: heap-buffer-overflow at {{.*}}library-scan.c:[[@LINE-6]]:{{[0-9]+}}: read of 17 bytes at
// STRSTR: weavec: unterminated-string at {{.*}}library-scan.c:[[@LINE-6]]:{{[0-9]+}}: read of {{[0-9]+}} bytes at

// IR-LABEL: define {{.*}}@main(
// IR: [[END:%[0-9a-z.]+]] = {{(tail )?}}call i64 @strcspn(
// IR: __weavec_rt_range{{(_ok)?}}
