/* RFC 0022: checked interfaces across translation units.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
int read_value(void *);
int main(void) {
  int x = 7;
  return read_value(&x);
}
