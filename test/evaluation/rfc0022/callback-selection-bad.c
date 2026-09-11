/* RFC 0022: frozen checked interface evaluation.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
static void a(char *p) {
  *p = 1;
}
static void b(char *p) {
  (void)p;
}
int main(int argc, char **argv) {
  (void)argv;
  void (*fn)(char *) = argc > 1 ? a : b;
  char x;
  fn(&x);
  return x;
}
