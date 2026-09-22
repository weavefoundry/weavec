// RFC 0030 §7.4: an array fill says what the cells held when it ran.
// STAGE: S8
// The first loop fills every cell with null, which the engine records as a
// contiguous fill. The second loop stores into a cell and reads it back; the
// two selectors need not be the same spelling of the same index, so applying
// the fill to the cell the read selects would put the null back and make the
// dereference a definite `null-dereference`. A store into the array settles
// that: the fill is joined with what the cell holds instead of replacing it.
// CLEAN
#include <stdlib.h>

struct node {
  int mark;
};

struct table {
  struct node **slot;
};

static struct node pool[4];

static struct node *fresh(int i) { return &pool[i & 3]; }

static void load(struct table *t, int n) {
  int i;
  for (i = 0; i < n; i++)
    t->slot[i] = NULL;
  for (i = 0; i < n; i++) {
    t->slot[i] = fresh(i);
    t->slot[i]->mark = i;
  }
}

int main(void) {
  struct table t;
  t.slot = malloc(4 * sizeof *t.slot);
  if (!t.slot)
    return 1;
  load(&t, 4);
  free(t.slot);
  return pool[3].mark == 3 ? 0 : 1;
}
