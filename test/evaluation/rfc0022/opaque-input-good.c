/* RFC 0022: frozen checked interface evaluation.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
static int get(void *p) {
  return *(int *)p;
}
int main(void) {
  int x = 7;
  return get(&x);
}
