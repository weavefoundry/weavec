/* RFC 0019 evaluation adapter: bind Jansson allocation to libc.
 * Configurable allocator hooks are outside this selected interface.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include <stdlib.h>
void *jsonp_malloc(size_t size) { return malloc(size); }
void jsonp_free(void *pointer) { free(pointer); }
void *jsonp_realloc(void *pointer, size_t old_size, size_t size) {
  (void)old_size;
  if (size == 0) {
    free(pointer);
    return NULL;
  }
  return realloc(pointer, size);
}
