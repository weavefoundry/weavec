// RFC 0030 §7.1: 'p + 1' into an int[2], dereferenced by the callee, is in bounds.
// STAGE: S3
// 'a + 1' points at the last element of 'int a[2]'. The static callee's parameter kind is the
// join of its arguments, and the engine proves one element behind 'a + 1'; the exported
// callee relies on A1's Single default, which the argument meets. Neither dereference is
// checked against anything tighter than the array. No error, no trap.
// CLEAN
// ASAN
static int deref(const int *p) { return *p; }

int deref_ext(const int *p) { return p[0]; }

int second(void) {
  int a[2] = {1, 2};
  return deref(a + 1) + deref_ext(a + 1);
}

int main(void) { return second() == 4 ? 0 : 1; }
