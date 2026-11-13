// RFC 0042 §5: struct fields get their implicit annotations only where the
// guard passes run and remove them: not with -fweavec-checks=none, not on a
// 32-bit target, and never left in the IR of a 64-bit enforcing compile.
// RUN: %weavec_cc -O2 -fweavec-checks=none -S -emit-llvm %s -o - | FileCheck %s
// RUN: %weavec_cc -O2 --target=i686-linux-gnu -S -emit-llvm %s -o - | FileCheck %s
// RUN: %weavec_cc -O2 -S -emit-llvm %s -o - | FileCheck %s
// CHECK-NOT: llvm.ptr.annotation
// CHECK-NOT: weavec.field

struct node { int *values; int count; };

int first(struct node *n) { return n->values[0]; }
