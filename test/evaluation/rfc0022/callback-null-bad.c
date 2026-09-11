/* RFC 0022: frozen checked interface evaluation.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
int main(void) {
  void (*fn)(char *) = 0;
  char x;
  fn(&x);
  return 0;
}
