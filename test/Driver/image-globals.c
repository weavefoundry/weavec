// RFC 0036 §4, RFC 0038 §1: a shared library's and a plug-in's globals get
// redzones that the process's runtime knows, so an overflow of one inside
// the image stops as it does in the executable.
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc %shared -O1 %S/Inputs/image-globals-lib.c -o %t/libimage%so
// RUN: %weavec_cc %if host-darwin %{ -bundle %} %else %{ -shared -fPIC %} -O1 %S/Inputs/image-globals-lib.c -o %t/image.bundle
// RUN: %weavec_cc -O1 %s -L%t -limage -Wl,-rpath,%t -o %t/prog
// RUN: %t/prog dylib 3
// RUN: not --crash %t/prog dylib 4 2>&1 | FileCheck %s
// RUN: %t/prog %t/image.bundle 3
// RUN: not --crash %t/prog %t/image.bundle 4 2>&1 | FileCheck %s
//
// CHECK: weavec: global-buffer-overflow at {{.*}}image-globals-lib.c:6:{{[0-9]+}}: read of 4 bytes
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

int lib_read(long i);

int main(int argc, char **argv) {
  long i = atol(argv[2]);
  if (strcmp(argv[1], "dylib") == 0)
    return lib_read(i) == 0;
  void *image = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (image == NULL)
    return 2;
  int (*read)(long) = (int (*)(long))dlsym(image, "lib_read");
  return read == NULL ? 2 : read(i) == 0;
}
