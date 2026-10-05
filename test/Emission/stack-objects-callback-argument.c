// RFC 0034 §4: a local is registered when a guard can reach its address.
// Handed to a library function that takes a callback (`pthread_create`'s
// argument, which the thread receives), it can: the callback may access it,
// or return it to code that does. The callback is named by a function, whose
// name decays to the pointer the parameter takes.
//
// RUN: %weavec_cc -O0 -Xclang -disable-llvm-passes -S -emit-llvm %s -o - | FileCheck %s

#include <pthread.h>

static void *echo(void *arg) { return arg; }

// CHECK-LABEL: define {{.*}}i32 @spawn(
// CHECK: %[[DATA:[0-9]+]] = alloca [8 x i32], align 16
// CHECK: call ptr @__weavec_stack_enter(ptr noundef %[[DATA]], i64 noundef 32
int spawn(void) {
  int data[8];
  pthread_t t;
  data[3] = 3;
  if (pthread_create(&t, 0, echo, &data[3]) != 0)
    return -1;
  void *result;
  pthread_join(t, &result);
  return *(int *)result;
}
