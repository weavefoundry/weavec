/* The ref/unref pair, unannotated: the share release and the count field
 * are inferred here and cross the unit boundary (RFC 0010, RFC 0005). */
#include "../../Inputs/prelude.h"
#include "counted.h"

struct counted *counted_new(void) {
  struct counted *c = malloc(sizeof *c);
  if (!c)
    return NULL;
  c->rc = 1;
  c->name = malloc(4);
  return c;
}

struct counted *counted_ref(struct counted *c) {
  c->rc++;
  return c;
}

void counted_unref(struct counted *c) {
  if (--c->rc == 0) {
    free(c->name);
    free(c);
  }
}
