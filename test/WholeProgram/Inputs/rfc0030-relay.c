// A caller that passes on its own parameters: the record keeps no constant
// for `m` and no width for `q`, so RFC 0030 §13.2 step 5 leaves `fill`'s
// requirement `unresolved(unknown-extent)` at this call.
int fill(int *p, int n);

int relay(int *q, int m) { return fill(q, m); }

int main(void) {
  int a[2] = {1, 2};
  return relay(a, 2) - 3;
}
