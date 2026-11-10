// RFC 0040 §6: WEAVEC_RT_REPORT_FORMAT=json writes each report as one line
// of JSON, to WEAVEC_RT_REPORT_LOG when it is set.
// RUN: %weavec_cc -O2 %s -o %t
// RUN: rm -f %t.log
// RUN: env WEAVEC_RT_REPORT_FORMAT=json WEAVEC_RT_REPORT_LOG=%t.log not --crash %t
// RUN: FileCheck %s < %t.log
#include <stdlib.h>
char *volatile sink;
int main(void) {
  char *p = malloc(8);
  sink = p;
  free(p);
  return sink[2];
}
// CHECK: {"weavec":41,"kind":"heap-use-after-free","proven":false,"site":{"file":"{{.*}}report-json.c","line":13,"column":{{[0-9]+}}},"what":"read of 1 bytes at 0x{{[0-9a-f]+}}","access":{"op":"read","width":1,"address":"0x{{[0-9a-f]+}}"},"thread":{"id":0},"stack":[{"pc":"0x{{[0-9a-f]+}}","module":"{{.*}}","offset":"0x{{[0-9a-f]+}}","function":"main","file":"{{.*}}report-json.c","line":13,
// CHECK-SAME: "object":{"memory":"heap","base":"0x{{[0-9a-f]+}}","size":8,"state":"released","released":[{"pc":"0x{{[0-9a-f]+}}",{{.*}}"line":12,
// CHECK-SAME: "allocated":[{"pc":"0x{{[0-9a-f]+}}",{{.*}}"line":10,
