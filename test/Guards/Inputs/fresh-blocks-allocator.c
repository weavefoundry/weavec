// The program's own allocator, compiled without weavec-cc: its blocks are
// not zeroes, so a program's own zeroing of one must stay.
#include <stddef.h>
#include <string.h>
static _Alignas(16) char heap[1 << 16];
static size_t used;
void *malloc(size_t size) {
  char *block = heap + used;
  used += (size + 15) & ~(size_t)15;
  if (used > sizeof heap)
    return NULL;
  memset(block, 0xAB, size);
  return block;
}
void *calloc(size_t count, size_t size) {
  void *block = malloc(count * size);
  if (block != NULL)
    memset(block, 0, count * size);
  return block;
}
void *realloc(void *block, size_t size) {
  void *moved = malloc(size);
  if (moved != NULL && block != NULL)
    memcpy(moved, block, size);
  return moved;
}
void free(void *block) { (void)block; }
