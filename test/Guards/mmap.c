// RFC 0035 §2.5: mmap and munmap go through the runtime, which clears the
// shadow of what they map and unmap.
// RUN: %weavec_cc -O2 -S -emit-llvm %s -o - | FileCheck %s
#include <sys/mman.h>

// CHECK: call ptr @__weavec_rt_mmap(
// CHECK: call i32 @__weavec_rt_munmap(
int roundtrip(void) {
  void *p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (p == MAP_FAILED)
    return 1;
  return munmap(p, 4096);
}
