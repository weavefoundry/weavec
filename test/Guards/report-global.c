// RFC 0040 §5.4: a global report names the global and where it is defined:
// its debug information's line under -g, else its unit.
// RUN: %weavec_cc -O2 %s -o %t && not --crash %t 2>&1 | FileCheck %s
// RUN: %weavec_cc -O2 -g %s -o %t.g && not --crash %t.g 2>&1 | FileCheck %s --check-prefix=LINE
int table[4] = {1, 2, 3, 4};
static int *opaque(int *p) {
  __asm__ volatile("" : "+r"(p));
  return p;
}
int main(int argc, char **argv) {
  (void)argv;
  return opaque(table)[argc + 3];
}
// CHECK: weavec: global-buffer-overflow at {{.*}}report-global.c:12:{{[0-9]+}}: read of 4 bytes
// CHECK: weavec: 0x{{[0-9a-f]+}} is 0 bytes after the 16-byte global 'table' defined at {{.*}}report-global.c
// LINE: 16-byte global 'table' defined at {{.*}}report-global.c:5
