/* RFC 0022: frozen checked interface evaluation.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
static void fill(char *p) {
  *p = 1;
}
static void invoke(void (*fn)(char *), char *p) {
  fn(p);
}
int main(void) {
  char x;
  invoke(fill, &x);
  return x;
}
