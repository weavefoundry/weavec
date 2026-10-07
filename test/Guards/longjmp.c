// RFC 0035 §3.5: frames abandoned by a longjmp leave no redzones a later
// frame could meet: a correct program that unwinds by longjmp and then uses
// the stack again does not trap.
// RUN: %weavec_cc -O2 %s -o %t
// RUN: %t | FileCheck %s
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static jmp_buf back;
void use(char *p) { p[0] = 1; }

__attribute__((noinline)) static void deep(int n) {
  char frame[100];
  use(frame);
  if (n == 0)
    longjmp(back, 1);
  deep(n - 1);
}

__attribute__((noinline)) static int scan(void) {
  char big[4096];
  memset(big, 7, sizeof big);
  int sum = 0;
  for (unsigned i = 0; i < sizeof big; i++)
    sum += big[i];
  return sum;
}

int main(void) {
  if (setjmp(back) == 0)
    deep(20);
  // CHECK: 28672
  printf("%d\n", scan());
  return 0;
}
