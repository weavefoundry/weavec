// RFC 0054 §1.2: a frame record of a frame that `longjmp` leaves goes where
// the `setjmp` returns, so a later frame that grows the stack past it is not
// answered with its type: a correct conversion of what is there now passes.
// RUN: %weavec_cc -O0 %s -o %t.o0
// RUN: %weavec_cc -O2 %s -o %t.o2
// RUN: %t.o0 | FileCheck %s
// RUN: %t.o2 | FileCheck %s
#include <setjmp.h>
#include <stdio.h>
#include <weavec.h>

struct circle { long kind; double radius; };
struct square { long kind; long side; };
static jmp_buf back;

WEAVEC_GUARDED __attribute__((noinline)) void leave(void *context) {
  (void)context;
  longjmp(back, 1);
}
// Records `c`, whose frame the longjmp leaves without its pop.
WEAVEC_SAFE __attribute__((noinline)) void deep(void) {
  struct circle c = {1, 2};
  leave(&c);
}
WEAVEC_SAFE __attribute__((noinline)) long side(void *context) {
  return ((struct square *)context)->side;
}
// `s`, which a guarded frame does not record, where `c` was.
WEAVEC_GUARDED __attribute__((noinline)) long later(void) {
  struct square s = {2, 3};
  return side(&s);
}

int main(void) {
  if (setjmp(back) == 0)
    deep();
  // CHECK: side 3
  printf("side %ld\n", later());
  return 0;
}
