// RFC 0030 §7.2: a constant 'T p[N]' parameter without 'static' is not a requirement.
// STAGE: S6
// C gives 'const int a[4]' no requirement and code commonly passes fewer elements, so the
// call passing one element is not checked against 4 (only a fix-it is suggested). 'first'
// reads a[0] only. No error, no trap.
// CLEAN
// ASAN
int first(const int a[4]) { return a[0]; }

int main(void) {
  int one[1] = {5};
  return first(one) == 5 ? 0 : 1;
}
