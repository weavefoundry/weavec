// RFC 0040 §5.6, §4.4: a fault prints its stack from the faulting
// instruction; without a symbolizer (WEAVEC_RT_SYMBOLIZE=0, one that is
// missing, one that never answers) frames name their module and offset,
// within the five seconds a report gives a symbolizer.
// RUN: %weavec_cc -O2 %s -o %t
// RUN: not --crash %t 2>&1 | FileCheck %s
// A symbolizer the environment names is the one asked (on Darwin, instead
// of atos), with each module's addresses as its file has them.
// RUN: env WEAVEC_RT_SYMBOLIZER=%weavec_cc not --crash %t 2>&1 | FileCheck %s
// RUN: env WEAVEC_RT_SYMBOLIZE=0 not --crash %t 2>&1 | FileCheck %s --check-prefix=PLAIN
// RUN: env WEAVEC_RT_SYMBOLIZER=/nonexistent/symbolizer not --crash %t 2>&1 | FileCheck %s --check-prefix=PLAIN-DRIVER
// RUN: printf '#!/bin/sh\nsleep 30\n' > %t.slow && chmod +x %t.slow
// RUN: env WEAVEC_RT_SYMBOLIZER=%t.slow not --crash %t 2>&1 | FileCheck %s --check-prefix=PLAIN-DRIVER
#include <stdint.h>

int *volatile wild = (int *)(uintptr_t)0x10000000;

__attribute__((noinline)) static int poke(void) { return *wild + 1; }

int main(void) { return poke() + 1; }
// CHECK: weavec: memory-fault at pc 0x{{[0-9a-f]+}}: {{read|access}} at 0x10000000
// CHECK-NEXT: #0 0x{{[0-9a-f]+}} in poke {{.*}}report-fault.c:18
// CHECK-NEXT: #1 0x{{[0-9a-f]+}} in main {{.*}}report-fault.c:20
// CHECK: weavec: summary: memory-fault at {{.*}}report-fault.c:18{{(:[0-9]+)?}} in poke (thread T0)

// PLAIN: #0 0x{{[0-9a-f]+}} in {{.*}} (/{{.*}}report-fault.c.tmp+0x{{[0-9a-f]+}})
// PLAIN-DRIVER: weavec: memory-fault at pc
