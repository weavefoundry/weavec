// RFC 0038 amendment 12: the runtime is a shared library, so its entry points
// are declared nonlazybind: a call goes through the GOT, bound at load time,
// and never through a lazy-binding resolver, which on Linux keeps only the
// argument registers where a preserve_all slow path promises its caller every
// register (http-parser's test, -O3, read a clobbered va_list).
// RUN: %weavec_cc -O2 -S -emit-llvm %s -o - | FileCheck %s
// RUN: %weavec_cc -O2 -S --target=aarch64-linux-gnu -fPIE %s -o - | FileCheck --check-prefix=ARM %s
// RUN: %weavec_cc -O2 -S --target=x86_64-linux-gnu -fPIE %s -o - | FileCheck --check-prefix=X86 %s

int get(int *p, long i) { return p[i]; }

// CHECK: declare {{.*}}void @__weavec_rt_guard_jump({{.*}}) [[ATTRS:#[0-9]+]]
// CHECK: attributes [[ATTRS]] = {{{.*}}nonlazybind
// ARM: ldr x{{[0-9]+}}, [x{{[0-9]+}}, :got_lo12:__weavec_rt_guard_jump]
// ARM: blr x
// X86: callq *__weavec_rt_guard_jump@GOTPCREL(%rip)
