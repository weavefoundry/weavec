// RFC 0037 §6: a fault the shadow did not see (here a volatile access to a
// constant address, which is never guarded) is reported once, and the
// program then dies of it as it would have; WEAVEC_RT_FAULTS=0 reports
// nothing.
// RUN: %weavec_cc -O2 %s -o %t
// RUN: not --crash %t 2>&1 | FileCheck %s
// RUN: env WEAVEC_RT_FAULTS=0 not --crash %t 2>&1 | FileCheck %s --check-prefix=NONE --allow-empty

// CHECK: weavec: memory-fault at pc 0x{{[0-9a-f]+}}: {{read|access}} at 0x1000
// NONE-NOT: weavec:
int main(void) { return *(volatile int *)0x1000; }
