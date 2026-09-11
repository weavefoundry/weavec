/* RFC 0022: frozen checked interface evaluation.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
static int get(void *p) {
  return *(int *)p;
}
int main(void) {
  int x;
  return get(&x);
}
