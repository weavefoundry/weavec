// RFC 0030 §7.3: a static callee given the cursor 'a + 4' does not rely on the Single default.
// STAGE: S6
// 'get' is static and its address is never taken, so its parameter's kind is the join of
// the argument kinds at its direct calls. 'a + 4' is neither Single-valid nor proven to have
// an element, so the kind is Unknown and p[i] is unresolved(unknown-extent), not proven. An
// early return precedes the access, so it is not a must-access and no call-site check
// covers it. The run reads a[4]; ASan reports it inside 'get'.
// RUN-INPUT:
// ASAN
static int get(const int *p, int i) {
  if (i < 0) return 0;
  return p[i]; // BUG: out-of-bounds // UNRESOLVED: spatial:unknown-extent
}

int main(int argc, char **argv) {
  int a[4] = {1, 2, 3, 4};
  (void)argv;
  return get(a, 0) + get(a + 4, argc - 1);
}
