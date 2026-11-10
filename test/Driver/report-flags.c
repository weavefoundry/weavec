// RFC 0040 §4.5, §8: an enforcing compile emits line tables unless the
// command line asks for debug information or ends in -g0, keeps the
// locals' names, and keeps frame pointers on x86-64 Linux; -fno-weavec and
// -fweavec-checks=none add nothing.
// RUN: %weavec_cc -### -O2 -c %s -o %t.o 2>&1 | FileCheck %s --check-prefix=DEFAULT
// RUN: %weavec_cc -### -O2 -g -c %s -o %t.o 2>&1 | FileCheck %s --check-prefix=FULL
// RUN: %weavec_cc -### -O2 -g0 -c %s -o %t.o 2>&1 | FileCheck %s --check-prefix=NONE
// RUN: %weavec_cc -### -O2 -fno-weavec -c %s -o %t.o 2>&1 | FileCheck %s --check-prefix=NONE
// RUN: %weavec_cc -### --target=x86_64-linux-gnu -O2 -c %s -o %t.o 2>&1 | FileCheck %s --check-prefix=X86
// RUN: %weavec_cc -### --target=x86_64-linux-gnu -fomit-frame-pointer -O2 -c %s -o %t.o 2>&1 | FileCheck %s --check-prefix=OMIT
// RUN: %weavec_cc -O2 -S -emit-llvm %s -o - | FileCheck %s --check-prefix=NAMES
void fill(int *values, int n);
int sum(int n) {
  int values[8] = {0};
  fill(values, n);
  return values[n & 7];
}
// DEFAULT: "-debug-info-kind=line-tables-only"
// DEFAULT-NOT: "-discard-value-names"
// FULL: "-debug-info-kind={{constructor|standalone}}"
// NONE-NOT: "-debug-info-kind=
// X86: "-mframe-pointer=all"
// OMIT: "-mframe-pointer=none"
// NAMES: @__weavec.file{{.*}} c"values\00"
