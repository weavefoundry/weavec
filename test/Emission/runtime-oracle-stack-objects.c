// RFC 0032, section 4.2: a local whose address leaves its function is entered into the
// thread's object list where it is declared, by the initialiser of a variable declared right
// after it, and left by that variable's cleanup when its scope ends. An object declared in a
// nested scope is entered with flag 2; a parameter is entered when the body starts; a local
// whose address stays (a subscript, a member) is not entered.
// The -O0 IR equals that of Inputs/runtime-oracle-stack-objects.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/runtime-oracle-stack-objects.expected.c %t -- -fweavec-runtime -fno-weavec-global-objects -Wno-weavec

extern void fill(char *p, unsigned long n);
extern void take(int *p);

int outer(void) {
  char buffer[16];
  int kept[4];
  fill(buffer, sizeof buffer);
  kept[0] = buffer[0];
  return kept[0];
}

int nested(int n) {
  int total = 0;
  if (n > 0) {
    int value = n;
    take(&value);
    total = value;
  }
  return total;
}

int parameter(int n) {
  take(&n);
  return n;
}
