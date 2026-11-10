// RFC 0040 §4.2, §4.4: weavec-cc --weavec-symbolize answers llvm-symbolizer's
// protocol from a module's line tables, and rewrites a saved report's
// frames that name only a module and an offset.
// REQUIRES: host-linux
// RUN: %weavec_cc -O0 -gline-tables-only %s -o %t
// RUN: sh -c 'echo "CODE \"$0\" 0x$(llvm-nm "$0" | grep " T target$" | cut -d" " -f1)"' %t \
// RUN:   | %weavec_cc --weavec-symbolize | FileCheck %s
// RUN: sh -c 'echo "    #0 0x1 in ? ($0+0x$(llvm-nm "$0" | grep " T target$" | cut -d" " -f1))"' %t > %t.log
// RUN: %weavec_cc --weavec-symbolize %t.log | FileCheck %s --check-prefix=LOG
int target(int x) { return x * 3; }
int main(int argc, char **argv) { (void)argv; return target(argc); }
// CHECK: target
// CHECK-NEXT: {{.*}}symbolize.c:10:
// CHECK-EMPTY:
// LOG: #0 0x1 in target {{.*}}symbolize.c:10:
