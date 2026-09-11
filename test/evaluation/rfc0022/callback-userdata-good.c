/* RFC 0022: frozen checked interface evaluation.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
static void fill(void *p) {
  *(int *)p = 7;
}
static void invoke(void (*fn)(void *), void *p) {
  fn(p);
}
int main(void) {
  int x;
  invoke(fill, &x);
  return x;
}
