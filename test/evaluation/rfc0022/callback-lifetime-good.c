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
int main(void) {
  int x = 7;
  invoke(keep, &x);
  int result = *(int *)saved;
  saved = 0;
  return result;
}
