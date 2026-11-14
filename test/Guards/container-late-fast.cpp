// RFC 0045 §9: a fast unit turns the container annotations off; a
// container a guarded image loaded before it annotated has its marks
// cleared then, so that its growth in that image is no container-overflow.
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cxx %shared -O1 %S/Inputs/container-late-fast-lib.cpp -o %t/libgrow%so
// RUN: %weavec_cxx -O1 -fweavec-safe %s -L%t -lgrow -Wl,-rpath,%t -o %t/prog
// RUN: %t/prog | FileCheck %s
#include <cstdio>
#include <weavec.h>

int grow();

// CHECK: 2
WEAVEC_GUARDED int main() {
  std::printf("%d\n", grow());
  return 0;
}
