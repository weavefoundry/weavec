// RFC 0033 §1, amendment 1: releasing a buffer whose copies the function left
// in the caller's struct, on a failure path, accesses nothing through them
// (msgpack-c's msgpack_vrefbuffer_init). A warning, not an error.
// STAGE: S8
// CLEAN
// ALLOW: conflicting-borrow
// RUN-INPUT: 1
// RUN-INPUT: 1 fail
#include <stdbool.h>
#include <stdlib.h>
struct buf { int *tail; int *end; int *array; void *head; };
static bool init(struct buf *b, int fail) {
  int *array = malloc(8 * sizeof *array);
  if (array == NULL) return false;
  b->tail = array;
  b->end = array + 8;
  b->array = array;
  void *chunk = fail ? NULL : malloc(64);
  if (chunk == NULL) {
    free(array); // the copies in `b` are never read: init failed
    return false;
  }
  b->head = chunk;
  return true;
}
int main(int argc, char **argv) {
  (void)argv;
  struct buf b;
  if (!init(&b, argc > 2)) return 0;
  free(b.array);
  free(b.head);
  return 0;
}
