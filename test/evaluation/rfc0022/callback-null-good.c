/* RFC 0022: frozen checked interface evaluation.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
static void set(char *p) {
  *p = 1;
}
int main(void) {
  void (*fn)(char *) = set;
  char x;
  fn(&x);
  return x;
}
