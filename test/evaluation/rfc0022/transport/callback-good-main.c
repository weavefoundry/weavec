/* RFC 0022: checked interfaces across translation units.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
void invoke(void (*)(char *), char *);
static void fill(char *p) {
  *p = 7;
}
static void skip(char *p) {
  (void)p;
}
int main(void) {
  char x;
  invoke(fill, &x);
  return x;
}
