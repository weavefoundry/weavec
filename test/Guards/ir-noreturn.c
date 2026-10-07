// RFC 0035 §3.5: before a call that does not return, the runtime clears the
// stack's shadow below the caller, so that the frames a longjmp abandons
// leave no redzones behind; exit and abort end the process and need none.
// RUN: %weavec_cc -O2 -S -emit-llvm %s -o - | FileCheck %s
#include <setjmp.h>
#include <stdlib.h>

jmp_buf there;

// CHECK-LABEL: define {{.*}}@leave(
// CHECK: call void @__weavec_rt_unpoison_stack()
// CHECK-NEXT: call void @longjmp(
void leave(void) { longjmp(there, 1); }

// CHECK-LABEL: define {{.*}}@quit(
// CHECK-NOT: __weavec_rt_unpoison_stack
// CHECK: call void @exit(
void quit(void) { exit(1); }
