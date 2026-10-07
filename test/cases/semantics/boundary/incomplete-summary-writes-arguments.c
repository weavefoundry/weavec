// RFC 0030 §5.5: an incomplete summary adds the unknown-callee default.
// STAGE: S7
// A caller applies an incomplete summary's effects plus the unknown-callee
// default on every pointer argument, which includes that the
// callee may write what the argument reaches. `fill` is over budget, so its
// summary is incomplete: `p`, which it sets through `&p`, is no longer the
// null the caller stored (a definite null dereference was reported here, as
// in sqlite's `memdbFromDbSchema` after `sqlite3_file_control`).
// FLAGS: -fweavec-budget=12
// RUN-INPUT: 1
// CLEAN
static int g = 7;

static int fill(int **out, int n) {
  int k = 0;
  for (int i = 0; i < n; i++) {
    if (i & 1)
      k++;
    else
      k--;
    if (k > 3)
      k = 0;
  }
  *out = &g;
  return k > 100;
}

int main(int argc, char **argv) {
  (void)argv;
  int *p = 0;
  if (fill(&p, argc))
    return 1;
  return *p == 7 ? 0 : 2;
}
