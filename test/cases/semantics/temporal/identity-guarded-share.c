// RFC 0030 §9.1: a guard conjunct a call cannot name widens the consume.
// STAGE: S8
// `attach` releases its own container when the value it is given is that
// container, which is the `json == value` shape. `build` passes a value that
// may be the container it is filling (`pick` can return its argument), so the
// guard cannot be refuted here; but it cannot be proved either, so the release
// of `held` is possible, never definite, and `drop(held)` is at most a
// possible double free.
// CLEAN
// ALLOW: double-free
#include <stdlib.h>

struct obj {
  unsigned long refs;
  struct obj *child;
};

static void drop(struct obj *o) {
  if (o && --o->refs == 0)
    free(o);
}

static struct obj *fresh(void) {
  struct obj *o = malloc(sizeof *o);
  if (o) {
    o->refs = 1;
    o->child = NULL;
  }
  return o;
}

static int attach(struct obj *holder, struct obj *value) {
  if (!value)
    return -1;
  if (!holder || holder == value) {
    drop(value);
    return -1;
  }
  holder->child = value;
  return 0;
}

static struct obj *pick(struct obj *other) {
  if (other)
    return other;
  return fresh();
}

int main(void) {
  struct obj *held = fresh();
  struct obj *spare = fresh();
  if (!held || !spare) {
    drop(held);
    drop(spare);
    return 1;
  }
  if (attach(held, pick(spare))) {
    drop(held);
    return 1;
  }
  drop(held->child);
  drop(held);
  return 0;
}
