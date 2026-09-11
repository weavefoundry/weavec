/* RFC 0022: checked interfaces across translation units.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include <stdlib.h>
static void *(*allocate)(size_t) = malloc;
static void (*release)(void *) = free;
void setup(void *(*a)(size_t), void (*f)(void *)) {
  allocate = a;
  release = f;
}
void *acquire(size_t n) {
  return (*allocate)(n);
}
void drop(void *p) {
  (*release)(p);
}
