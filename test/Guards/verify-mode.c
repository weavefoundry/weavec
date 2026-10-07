// RFC 0035 §7: verify mode keeps every guard a local rule removed, as a
// monitor whose site carries the proven flag (4).
// RUN: %weavec_cc -O2 -fweavec-checks=verify -S -emit-llvm %s -o - | FileCheck %s
// RUN: %weavec_cc -O2 -fweavec-checks=verify %s -o %t
// RUN: %t

// CHECK: @__weavec.site{{.*}} = private unnamed_addr constant { ptr, i32, i32, i32 } { ptr @__weavec.file{{.*}}, i32 {{[0-9]+}}, i32 {{[0-9]+}}, i32 4 }
int local(unsigned i) {
  int a[4] = {1, 2, 3, 4};
  return a[i & 3];
}

int main(int argc, char **argv) {
  (void)argv;
  return local((unsigned)argc) == 0;
}
