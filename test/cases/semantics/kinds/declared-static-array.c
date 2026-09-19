// RFC 0030 §7.2 (T p[static N]): the parameter is Counted(N) and non-null, required at every call.
// STAGE: S6
// An argument whose exact extent is below the declared requirement is a violation at the
// call (§3.3): an int[3] passed for [static 4] is a definite out-of-bounds error.
int sum4(const int a[static 4]) { return a[0] + a[3]; }

int three(void) {
  int b[3] = {1, 2, 3};
  return sum4(b); // BUG: out-of-bounds definite
}
