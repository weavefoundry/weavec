/* RFC 0022: frozen checked interface evaluation.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
static void set(void *p) {
  *(int *)p = 7;
}
int main(void) {
  const int x = 0;
  set((void *)&x);
  return x;
}
