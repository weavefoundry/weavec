// RFC 0038 §1: a static Linux program is one image, and takes the runtime's
// archive instead of its shared library, with no rpath; it still stops a
// heap overflow.
//
// REQUIRES: host-linux
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -### -static %s -o %t/prog 2>&1 | FileCheck --check-prefix=LINK %s
// RUN: %weavec_cc -O1 -static %s -o %t/prog
// RUN: %t/prog 4
// RUN: not --crash %t/prog 5 2>&1 | FileCheck --check-prefix=STOP %s
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  char *p = malloc(4);
  if (p == NULL || argc < 2)
    return 1;
  memset(p, 0, (size_t)atoi(argv[1]));
  free(p);
  return 0;
}

// LINK: "{{[^"]*}}libweavec_rt.a" "-lpthread"
// LINK-NOT: "-rpath"
// STOP: weavec: heap-buffer-overflow
