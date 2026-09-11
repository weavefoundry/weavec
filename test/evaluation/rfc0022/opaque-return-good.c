/* RFC 0022: frozen checked interface evaluation.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
static void *identity(void *p) {
  return p;
}
int main(void) {
  int x = 7;
  int *p = identity(&x);
  return *p;
}
