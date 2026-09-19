// RFC 0030, section 11: a target whose C library has no usable-size query
// cannot zero the heap family, so weavec-cc says so once per invocation and
// leaves its allocators alone; locals and alloca are still zero-initialised.
//
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %weavec_cc --target=x86_64-pc-windows-msvc -S -emit-llvm -Xclang -disable-llvm-passes %s %S/Inputs/zero-init-second.c 2>&1 | FileCheck --check-prefix=ONCE %s
// RUN: FileCheck --check-prefix=IR %s < %t/rfc0030-zero-init-target.ll
// RUN: %weavec_cc --target=x86_64-unknown-linux-gnu -S -emit-llvm -Xclang -disable-llvm-passes %s -o %t/linux.ll 2>&1 | FileCheck --allow-empty --check-prefix=QUIET %s
// RUN: FileCheck --check-prefix=LINUX %s < %t/linux.ll
//
// ONCE: weavec-cc: warning: the C library of 'x86_64-pc-windows-msvc{{[^']*}}' has no usable-size query, so heap allocations are not zero-initialised
// ONCE-NOT: usable-size query
// QUIET-NOT: usable-size query
// IR: call void @llvm.memset
// IR: call ptr @malloc(
// IR-NOT: _zero(
// LINUX: call ptr @__weavec_malloc_zero(

void *malloc(__typeof__(sizeof 0));

char *make(__typeof__(sizeof 0) n) {
  char *p = __builtin_alloca(n);
  p[0] = 1;
  return malloc(n);
}
