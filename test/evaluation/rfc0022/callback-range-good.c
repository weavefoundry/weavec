/* RFC 0022: frozen checked interface evaluation.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
static void fill(char *p, unsigned n) {
  for (unsigned i = 0; i < n; ++i)
    p[i] = 1;
}
static void invoke(void (*fn)(char *, unsigned), char *p, unsigned n) {
  fn(p, n);
}
int main(void) {
  char a[8];
  invoke(fill, a, 8);
  return a[7];
}
