// RFC 0030, sections 10.2, 10.7 and 17.4: with -fweavec-checks=report every
// check helper receives its site's file, line and column, and a failure calls
// __weavec_rt_report, which prints
//   weavec: runtime check failed: <template> at <file>:<line>:<column>
// once per site and returns (no guarantee), or aborts under WEAVEC_RT_ABORT=1.
// weavec-cc links libweavec_rt.a itself when the link is given the flag. The
// case runner and gate G11 attribute traps to lines and templates this way.
//
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -O1 -fweavec-checks=report -Wno-error=weavec-null-dereference %s -o %t/report
// RUN: %t/report 2>&1 | FileCheck --check-prefix=CLEAN %s
// RUN: %t/report all 2>&1 | FileCheck --check-prefix=REPORT %s
// RUN: env WEAVEC_RT_ABORT=1 not --crash %t/report index 2>&1 | FileCheck --check-prefix=ABORT %s
//
// Compiled and linked separately: the link alone adds the runtime.
// RUN: %weavec_cc -O1 -fweavec-checks=report -Wno-error=weavec-null-dereference -c %s -o %t/report.o
// RUN: %weavec_cc -fweavec-checks=report %t/report.o -o %t/linked
// RUN: %t/linked all 2>&1 | FileCheck --check-prefix=REPORT %s
// RUN: %weavec_cc -fweavec-checks=report -### %t/report.o -o %t/linked 2>&1 | FileCheck --check-prefix=LINK %s
// RUN: %weavec_cc -### %t/report.o -o %t/linked 2>&1 | FileCheck --check-prefix=TRAPLINK %s
//
// CLEAN-NOT: runtime check failed
// CLEAN: clean: 8
// ABORT: weavec: runtime check failed: index at {{.*}}rfc0030-report-runtime.c:{{[0-9]+}}:{{[0-9]+}}
// ABORT-NOT: done
// LINK: libweavec_rt.a
// TRAPLINK-NOT: libweavec_rt.a

#include <stdio.h>
#include <string.h>
#include <weavec.h>

struct pair {
  int a, b;
};

volatile int Enabled = 0;
volatile int Index = 2;

// `&p->b` only computes an address, so the program survives the report.
// REPORT: weavec: runtime check failed: nonnull at {{.*}}rfc0030-report-runtime.c:[[#@LINE+1]]:46
static int *second(struct pair *p) { return &p->b; }
static int pick(int i) {
  int table[4] = {1, 2, 3, 4};
  // REPORT-NEXT: weavec: runtime check failed: index at {{.*}}rfc0030-report-runtime.c:[[#@LINE+1]]:10
  return table[i];
}
static int stack(int n, int i) {
  int v[n];
  for (int k = 0; k < n; ++k)
    v[k] = k;
  // REPORT-NEXT: weavec: runtime check failed: span at {{.*}}rfc0030-report-runtime.c:[[#@LINE+1]]:10
  return v[i];
}
static int positive(int n) {
  // REPORT-NEXT: weavec: runtime check failed: assert at {{.*}}rfc0030-report-runtime.c:[[#@LINE+1]]:3
  WEAVEC_ASSUME(n > 0);
  return n;
}
// Each site reports once, though every check fails twice.
// REPORT-NEXT: all: done

int main(int argc, char **argv) {
  const char *what = argc > 1 ? argv[1] : "";
  struct pair pair = {1, 2};
  struct pair *maybe = Enabled ? &pair : 0;
  if (strcmp(what, "index") == 0) {
    pick(Index + 5);
    puts("index: done");
    return 0;
  }
  if (strcmp(what, "all") == 0) {
    for (int round = 0; round < 2; ++round) {
      second(maybe);
      pick(Index + 5);
      stack(Index, Index + 1);
      positive(Index - 2);
    }
    fprintf(stderr, "all: done\n");
    return 0;
  }
  printf("clean: %d\n",
         *second(&pair) + pick(Index) + stack(3, Index) + positive(1));
  return 0;
}
