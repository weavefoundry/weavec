// The program's own allocator, compiled without weavec-cc: its `free` stops
// at a block it did not allocate.
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
static char heap[1 << 16];
static size_t used;
void *malloc(size_t size) {
  void *block = heap + used;
  used += (size + 15) & ~(size_t)15;
  return used <= sizeof heap ? block : NULL;
}
void *calloc(size_t count, size_t size) { return malloc(count * size); }
void *realloc(void *block, size_t size) { (void)block; return malloc(size); }
void free(void *block) {
  if (block != NULL && ((char *)block < heap || (char *)block >= heap + sizeof heap)) {
    fprintf(stderr, "allocator: a foreign block\n");
    abort();
  }
}
// Code the runtime does not see releases what it is given.
void release(void *block) { free(block); }
