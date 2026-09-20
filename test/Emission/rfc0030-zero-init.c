// RFC 0030, section 11: in the enforcing modes weavec-cc compiles with
// -ftrivial-auto-var-init=zero and lowers the zero-init rows of the library
// table, so that every byte a program can read from a local or a lowered
// block is zero or was written by it: a pointer read from such storage is null
// and its dereference traps. -fno-weavec-zero-init and -fweavec-checks=none
// turn both off. A unit that defines an allocator lowers nothing. The ledger
// counts what is not lowered (summary.a5).
//
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -O2 -Wno-error=weavec %s -o %t/zero
// RUN: %t/zero | FileCheck --check-prefix=ZEROED %s
// RUN: %t/zero uninitialised 2>&1 | FileCheck --check-prefix=TRAPPED %s
//
// The IR before the always-inliner shows which helper each call became.
// RUN: %weavec_cc -O0 -S -emit-llvm -Xclang -disable-llvm-passes -Wno-error=weavec %s -o %t/on.ll
// RUN: FileCheck --check-prefix=ON %s < %t/on.ll
// RUN: %weavec_cc -O0 -S -emit-llvm -Xclang -disable-llvm-passes -Wno-error=weavec -fno-weavec-zero-init %s -o %t/off.ll
// RUN: FileCheck --check-prefix=OFF %s < %t/off.ll
// RUN: %weavec_cc -O0 -S -emit-llvm -Xclang -disable-llvm-passes -Wno-error=weavec -fweavec-checks=none %s -o %t/none.ll
// RUN: FileCheck --check-prefix=OFF %s < %t/none.ll
// RUN: %weavec_cc -O0 -S -emit-llvm -Xclang -disable-llvm-passes -Wno-error=weavec -DALLOCATOR %s -o %t/allocator.ll
// RUN: FileCheck --check-prefix=ALLOCATOR %s < %t/allocator.ll
//
// RUN: %weavec_cc -fsyntax-only -Wno-error=weavec -fweavec-ledger=%t/on.json %s
// RUN: FileCheck --check-prefix=LEDGER-ON %s < %t/on.json
// RUN: %weavec_cc -fsyntax-only -Wno-error=weavec -fno-weavec-zero-init -fweavec-ledger=%t/off.json %s
// RUN: FileCheck --check-prefix=LEDGER-OFF %s < %t/off.json
//
// ZEROED: zeroed: malloc 0 calloc 0 realloc 0 alloca 0 strdup 0 local 0
// TRAPPED: signal: {{SIGTRAP|SIGILL}}
//
// ON-DAG: !annotation
// ON-DAG: call ptr @__weavec_malloc_zero(
// ON-DAG: call ptr @__weavec_realloc_zero(
// ON-DAG: call ptr @__weavec_strdup_zero(
// OFF-NOT: !annotation
// OFF-NOT: _zero(
// ALLOCATOR-NOT: call {{.*}}_zero(
//
// One allocation call is not lowered (valloc's address is taken, and it has
// no same-signature wrapper), and one pointer local can be jumped over.
// LEDGER-ON: "a5": {
// LEDGER-ON-NEXT: "nonLoweredAllocations": 1,
// LEDGER-ON-NEXT: "bypassedDeclarations": 1
// LEDGER-OFF: "a5": {
// LEDGER-OFF-NEXT: "nonLoweredAllocations": 8,

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#define USABLE(p) malloc_size(p)
#else
#include <malloc.h>
#define USABLE(p) malloc_usable_size(p)
#endif

volatile int Size = 32;

#ifdef ALLOCATOR
// A unit that defines the allocator itself is never wrapped into recursion.
void *malloc(size_t n) { return calloc(1, n); }
#endif

static int last(const char *p, int n) { return p[n - 1]; }

// Reads a pointer local that nothing wrote: zero-initialised, so null.
static int uninitialised(void) {
  int *p;
  if (Size < 0)
    p = 0;
  return *p;
}

// A declaration a jump can bypass (A5).
static int bypass(int k) {
  switch (k) {
    int *q;
  case 1:
    q = 0;
    return q == 0;
  default:
    return 0;
  }
}

static void onSignal(int signal) {
  (void)signal;
  write(2, "signal: SIGTRAP\n", 16);
  _exit(0);
}

void *(*Other)(size_t) = valloc;

int main(int argc, char **argv) {
  signal(SIGTRAP, onSignal);
  signal(SIGILL, onSignal);
  if (argc > 1 && strcmp(argv[1], "uninitialised") == 0)
    return uninitialised();
  // Write garbage into freed blocks, so that a reuse would show it.
  for (int i = 0; i < 64; ++i) {
    char *junk = malloc((size_t)Size);
    memset(junk, 0x5a, (size_t)Size);
    free(junk);
  }
  char *m = malloc((size_t)Size);
  char *c = calloc(1, (size_t)Size);
  char *r = malloc(8);
  memset(r, 'r', 8);
  r = realloc(r, (size_t)Size * 4);
  int n = Size;
  char *a = __builtin_alloca(n);
  char *d = strdup("abc");
  char local[16];
  printf("zeroed: malloc %d calloc %d realloc %d alloca %d strdup %d local %d\n",
         last(m, Size), last(c, Size), last(r, Size * 4), last(a, Size),
         last(d, (int)USABLE(d)), last(local, 16) + bypass(2));
  free(m);
  free(c);
  free(r);
  free(d);
  (void)Other;
  return 0;
}
