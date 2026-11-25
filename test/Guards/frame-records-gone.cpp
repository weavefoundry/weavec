// RFC 0054 §1.2: a frame record of a frame an exception unwinds without a
// cleanup goes where the exception is caught, so a later frame that grows
// the stack past it is not answered with its type.
// RUN: %weavec_cxx -O0 %s -o %t.o0
// RUN: %weavec_cxx -O2 %s -o %t.o2
// RUN: %t.o0 | FileCheck %s
// RUN: %t.o2 | FileCheck %s
#include <cstdio>
#include <weavec.h>

struct Circle { long kind; double radius; };
struct Square { long kind; long side; };

WEAVEC_GUARDED __attribute__((noinline)) void leave(void *context) {
  (void)context;
  throw 1;
}
WEAVEC_SAFE __attribute__((noinline)) void deep() {
  Circle c = {1, 2};
  leave(&c);
}
WEAVEC_SAFE __attribute__((noinline)) long side(void *context) {
  return static_cast<Square *>(context)->side;
}
WEAVEC_GUARDED __attribute__((noinline)) long later() {
  Square s = {2, 3};
  return side(&s);
}

int main() {
  try {
    deep();
  } catch (int) {
  }
  // CHECK: side 3
  std::printf("side %ld\n", later());
  return 0;
}
