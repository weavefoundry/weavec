// RFC 0037 §6: a SIGSEGV or SIGBUS another process or the program sends
// (kill, raise) faulted nothing: it is not reported, and it ends the
// program as it would have, rather than being taken for a fault that
// retries.
// RUN: %weavec_cc -O2 %s -o %t
// RUN: not --crash %t segv 2>&1 | FileCheck %s --allow-empty
// RUN: not --crash %t bus 2>&1 | FileCheck %s --allow-empty

// CHECK-NOT: weavec:
// CHECK-NOT: survived
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "bus") == 0)
    (void)kill(getpid(), SIGBUS);
  else
    (void)raise(SIGSEGV);
  fputs("survived\n", stderr);
  return 0;
}
