// RFC 0042 §9, M3 (amendment 21): a typed function that holds a pointer to a
// released block reads that block's bytes and believes the pointers in it.
// In a program with typed code the sweep clears a released block that
// something still refers to, so its pointers read null: the chain never
// reaches a block reused meanwhile, however many sweeps reuse blocks of the
// same size (each written -1).
// CLEAN
// FLAGS: -O1 -fweavec-safe
#include <weavec.h>
#include <stdlib.h>

struct node { struct node *next; long value; };
struct anchor { struct node *head; };

static struct anchor kept;

long through(const struct anchor *a) {
  const struct node *second = a->head->next;
  return second != NULL ? second->value : 0;
}

WEAVEC_GUARDED int main(void) {
  struct node *first = calloc(1, sizeof *first);
  struct node *second = calloc(1, sizeof *second);
  first->next = second;
  second->value = 42;
  kept.head = first;
  free(second);
  free(first);
  for (long i = 0; i < 16L << 20; i++) {
    long *block = malloc(sizeof(struct node));
    block[0] = block[1] = -1;
    free(block);
  }
  const long value = through(&kept);
  if (value != 0 && value != 42)
    abort();
  return 0;
}
