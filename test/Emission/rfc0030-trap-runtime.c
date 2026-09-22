// RFC 0030, sections 10.2, 10.6 and 10.7: in the default trap mode each
// inserted check traps when it fails and costs nothing observable when it
// holds. Each failing input has a correct twin that runs clean. A failed
// check is detected by its signal, which the program catches: an uncaught
// one costs seconds per process for the crash report on macOS.
//
// The null inputs are null only at run time; the analysis may still report
// them, which does not change a check.
//
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -O2 -Wno-error=weavec-null-dereference %s -o %t/trap
// RUN: %t/trap | FileCheck --check-prefix=CLEAN %s
// RUN: %t/trap nonnull 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/trap member 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/trap callee 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/trap index 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/trap index-negative 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/trap span 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/trap len 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/trap sprintf 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/trap assert 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/trap copy-null 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/trap copy-empty 2>&1 | FileCheck --check-prefix=EMPTY %s
// RUN: %t/trap fold 2>&1 | FileCheck --check-prefix=TRAPPED %s
//
// The same at -O0, where every helper is still inlined.
// RUN: %weavec_cc -O0 -Wno-error=weavec-null-dereference %s -o %t/trap0
// RUN: %t/trap0 | FileCheck --check-prefix=CLEAN %s
// RUN: %t/trap0 span 2>&1 | FileCheck --check-prefix=TRAPPED %s
// RUN: %t/trap0 index 2>&1 | FileCheck --check-prefix=TRAPPED %s
//
// CLEAN: clean: 1 2 3 4 5 6 7 8
// TRAPPED: signal: {{SIGTRAP|SIGILL}}
// TRAPPED-NOT: survived
// EMPTY: copy-empty: survived

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <weavec.h>

struct node {
  int value;
  struct node *next;
};

// Inputs the analysis cannot see through (external, volatile), so that
// nothing is decided statically.
struct node Node = {1, 0};
struct node *volatile NodeInput = &Node;
int (*volatile CalleeInput)(int);
volatile int IndexInput = 2;
volatile int CountInput = 4;
// Zero, but only at run time: the null inputs below are "may be null".
volatile int Enabled = 0;

static int twice(int x) { return 2 * x; }
static int value(struct node *n) { return n->value; }
static int first(int *p) { return *p; }
static int call(int (*f)(int), int x) { return f(x); }
static int pick(int i) {
  int table[4] = {1, 2, 3, 4};
  return table[i];
}
static int stack(int n, int i) {
  int v[n];
  for (int k = 0; k < n; ++k)
    v[k] = k + 1;
  return v[i];
}
static void fill(unsigned long n) {
  char d[8];
  memset(d, 'x', n);
  if (d[0] != 'x')
    puts("unreachable");
}
static int format(int x) {
  char buf[4];
  return sprintf(buf, "%d", x);
}
static int positive(int n) {
  WEAVEC_ASSUME(n > 0);
  return n;
}
static void copy(char *d, const char *s, unsigned long n) { memcpy(d, s, n); }
// Section 8.3: a zero-length memcpy accepts a null pointer, and the optimiser
// must not take the call as proof that the pointer is not null.
static int fold(int *p, const int *x) {
  memcpy(p, x, 0);
  return *p;
}

static void onSignal(int signal) {
  const char *text = signal == SIGTRAP ? "signal: SIGTRAP\n" : "signal: SIGILL\n";
  write(2, text, strlen(text));
  _exit(0);
}

int main(int argc, char **argv) {
  signal(SIGTRAP, onSignal);
  signal(SIGILL, onSignal);
  CalleeInput = twice;
  const char *what = argc > 1 ? argv[1] : "";
  char buffer[4] = "abc";
  struct node *nullNode = Enabled ? &Node : 0;
  int *nullInt = Enabled ? &Node.value : 0;
  int (*nullCallee)(int) = Enabled ? twice : 0;
  char *nullChars = Enabled ? buffer : 0;
  if (strcmp(what, "nonnull") == 0)
    first(nullInt);
  else if (strcmp(what, "member") == 0)
    value(nullNode);
  else if (strcmp(what, "callee") == 0)
    call(nullCallee, 1);
  else if (strcmp(what, "index") == 0)
    pick(IndexInput + 2);
  else if (strcmp(what, "index-negative") == 0)
    pick(IndexInput - 3);
  else if (strcmp(what, "span") == 0)
    stack(CountInput, CountInput);
  else if (strcmp(what, "len") == 0)
    fill((unsigned long)CountInput * 3);
  else if (strcmp(what, "sprintf") == 0)
    format(IndexInput * 1000);
  else if (strcmp(what, "assert") == 0)
    positive(IndexInput - 2);
  else if (strcmp(what, "copy-null") == 0)
    copy(nullChars, buffer, (unsigned long)CountInput - 3);
  else if (strcmp(what, "fold") == 0)
    fold(nullInt, &Node.value);
  else if (strcmp(what, "copy-empty") == 0) {
    copy(nullChars, nullChars, (unsigned long)CountInput - 4);
    puts("copy-empty: survived");
    return 0;
  } else {
    fill((unsigned long)CountInput);
    printf("clean: %d %d %d %d %d %d %d %d\n", value(NodeInput),
           first(&Node.value) + 1, call(CalleeInput, 1) + 1,
           pick(IndexInput + 1), stack(CountInput, CountInput - 1) + 1,
           format(IndexInput * 3) + 5, positive(7), fold(&Node.value, &Node.value) + 7);
    return 0;
  }
  puts("survived");
  return 0;
}
