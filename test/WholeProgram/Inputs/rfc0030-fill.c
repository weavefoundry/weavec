// An exported function whose body must-accesses `p[i]` under `i < n`, so
// RFC 0030 §7.5 R2 infers `0 < n -> counted(n)` for `p`. Inside the body
// the access is `trusted(caller-contract)`; the requirement is exported in
// the record and decided at every caller the program contains (§13.2 step
// 5).
int fill(int *p, int n) {
  int total = 0;
  for (int i = 0; i < n; ++i)
    total += p[i];
  return total;
}
