/* RFC 0022: frozen checked interface evaluation.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
struct cb {
  void (*fn)(char *);
};
static void set(char *p) {
  (void)p;
}
int main(void) {
  struct cb a = {set}, b = a;
  char x;
  b.fn(&x);
  return x;
}
