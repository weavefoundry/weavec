// RFC 0030, section 10.9: injecting the prelude would change the predefines
// of a precompiled header built by plain Clang, so with -include-pch the
// check helpers are declared `extern` in the AST instead, and weavec-cc links
// libweavec_chk.a, which defines them out of line. The checks cost a call;
// they trap all the same. Report mode calls the `_report` helpers, which come
// with libweavec_rt.a.
//
// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang -x c-header -O0 -D__WEAVEC__=1 -isystem %resource_dir %S/Inputs/pch-header.h -o %t/header0.pch
// RUN: %clang -x c-header -O2 -D__WEAVEC__=1 -isystem %resource_dir %S/Inputs/pch-header.h -o %t/header2.pch
// RUN: %weavec_cc -Wno-error=weavec-null-dereference -include-pch %t/header0.pch -O0 -S -emit-llvm %s -o %t/pch.ll
// RUN: FileCheck --check-prefix=IR %s < %t/pch.ll
// RUN: %weavec_cc -Wno-error=weavec-null-dereference -include-pch %t/header2.pch -O2 %s -o %t/pch
// RUN: %t/pch | FileCheck --check-prefix=CLEAN %s
// RUN: %t/pch null 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/pch index 2>&1 | FileCheck --check-prefix=TRAPPED %s
//
// The header precompiled by weavec-cc itself behaves the same.
// RUN: %weavec_cc -x c-header -O2 %S/Inputs/pch-header.h -o %t/weavec.pch
// RUN: %weavec_cc -Wno-error=weavec-null-dereference -include-pch %t/weavec.pch -O2 %s -o %t/pch2
// RUN: %t/pch2 null 2>&1 | FileCheck --check-prefix=TRAPPED %s
//
// RUN: %weavec_cc -Wno-error=weavec-null-dereference -include-pch %t/header2.pch -fweavec-checks=report -O2 %s -o %t/report
// RUN: %t/report null-address 2>&1 | FileCheck --check-prefix=REPORT %s
//
// IR-DAG: declare ptr @__weavec_chk_nonnull(ptr noundef)
// IR-DAG: declare i64 @__weavec_chk_index(i64 noundef, i64 noundef)
// IR-DAG: call ptr @__weavec_chk_nonnull(
// IR-NOT: define {{.*}}@__weavec_chk_
// CLEAN: clean: 3
// TRAPPED: signal: {{SIGTRAP|SIGILL}}
// TRAPPED-NOT: survived
// REPORT: weavec: runtime check failed: nonnull at {{.*}}rfc0030-pch-fallback.c:{{[0-9]+}}:{{[0-9]+}}

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

volatile int Enabled = 0;
volatile int Index = 1;

static int pick(int i) {
  int table[3] = {1, 2, 3};
  return table[i];
}

static int *address(struct item *item) { return &item->value; }

static void onSignal(int signal) {
  (void)signal;
  write(2, "signal: SIGTRAP\n", 16);
  _exit(0);
}

int main(int argc, char **argv) {
  signal(SIGTRAP, onSignal);
  signal(SIGILL, onSignal);
  struct item one = {1, 0};
  struct item *maybe = Enabled ? &one : 0;
  const char *what = argc > 1 ? argv[1] : "";
  if (strcmp(what, "null") == 0)
    value_of(maybe);
  else if (strcmp(what, "index") == 0)
    pick(Index + 5);
  else if (strcmp(what, "null-address") == 0)
    address(maybe);
  else {
    printf("clean: %d\n", value_of(&one) + pick(Index));
    return 0;
  }
  puts("survived");
  return 0;
}
