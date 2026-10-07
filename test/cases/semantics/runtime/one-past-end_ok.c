// RFC 0032 §3 (One past the end): a subscript's base may be one past the end of its object, on the heap, the stack and among globals.
// STAGE: S4
// 'last' reads 'end[-1]'. On the heap the byte after a block belongs to no object, so 'end'
// still identifies the block. For a stack or global object another object may start exactly
// at 'end': the guard then takes the object before it.
// CLEAN
// RUN-INPUT:
// ASAN
#include <stdlib.h>
static char first[16];
static char second[16];
struct span { const char *begin, *end; };
static int last(const struct span *s) {
  return s->end[-1];
}
static int count(const struct span *s) {
  int n = 0;
  for (const char *p = s->begin; p != s->end; p++)
    n += *p != 0;
  return n;
}
int main(void) {
  char local[16];
  char *heap = malloc(16);
  struct span spans[3];
  if (!heap) return 1;
  for (int i = 0; i < 16; i++) {
    first[i] = 'a';
    second[i] = 'b';
    local[i] = 'c';
    heap[i] = 'd';
  }
  spans[0].begin = first;
  spans[0].end = first + 16;
  spans[1].begin = local;
  spans[1].end = local + 16;
  spans[2].begin = heap;
  spans[2].end = heap + 16;
  int r = 0;
  for (int i = 0; i < 3; i++)
    r += last(&spans[i]) == 0 || count(&spans[i]) != 16;
  free(heap);
  return r + (second[0] != 'b');
}
