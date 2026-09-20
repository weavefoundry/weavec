// RFC 0030, sections 10.2 and 10.7: verify mode, the soundness monitor.
// Unproven facets get the __weavec_chk_* checks with the trap category
// "weavec", and checks of proven facets that have a witness use the
// __weavec_prv_* family, whose category "weavec.proven" says the engine proved
// something false. The engine of this stage publishes no proof, so every check
// here is of the first kind (a unit test covers the second); the IR before
// the always-inliner names the helper of each check. A verify build runs its
// correct inputs without a trap and traps on the others.
//
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -fweavec-checks=verify -O0 -S -emit-llvm -Xclang -disable-llvm-passes %s -o %t/verify.ll
// RUN: FileCheck --check-prefix=IR %s < %t/verify.ll
// RUN: %weavec_cc -fweavec-checks=verify -O2 %s -o %t/verify
// RUN: %t/verify | FileCheck --check-prefix=CLEAN %s
// RUN: %t/verify index 2>&1 | FileCheck --check-prefix=TRAPPED %s
//
// IR-DAG: call ptr @__weavec_chk_nonnull(
// IR-DAG: call i64 @__weavec_chk_index(
// IR-DAG: define internal ptr @__weavec_chk_nonnull(
// IR-NOT: call {{.*}}@__weavec_prv_
// CLEAN: clean: 3
// TRAPPED: signal: {{SIGTRAP|SIGILL}}
// TRAPPED-NOT: survived

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

volatile int Index = 1;

static int pick(int *p, int i) {
  int table[4] = {1, 2, 3, 4};
  return *p + table[i];
}

static void onSignal(int signal) {
  const char *text = signal == SIGTRAP ? "signal: SIGTRAP\n" : "signal: SIGILL\n";
  write(2, text, strlen(text));
  _exit(0);
}

int main(int argc, char **argv) {
  signal(SIGTRAP, onSignal);
  signal(SIGILL, onSignal);
  int one = 1;
  if (argc > 1 && strcmp(argv[1], "index") == 0) {
    pick(&one, Index + 7);
    puts("survived");
    return 0;
  }
  printf("clean: %d\n", pick(&one, Index));
  return 0;
}
