/* RFC 0022: checked interfaces across translation units.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
void invoke(void (*fn)(char *), char *p) {
  fn(p);
}
