/* RFC 0022: frozen checked interface evaluation.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
static void *saved;
static void keep(void *p) {
  saved = p;
}
static void invoke(void (*fn)(void *), void *p) {
  fn(p);
}
static void make(void) {
  int x = 7;
  invoke(keep, &x);
}
int main(void) {
  make();
  return *(int *)saved;
}
