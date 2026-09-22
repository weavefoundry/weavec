// RFC 0030 §5.1: a destructor declared only 'nonnull' is still an unknown callee.
// STAGE: S3
// Nullability and extent attributes never lift the temporal default; only ownership
// contracts do (the Departure of §5.1). 'obj_destroy' may therefore have freed 'o': its
// Call site and the later read are unresolved(unknown-callee) instead of proven as if the
// call only borrowed 'o'. There is no diagnostic.
// CLEAN
#include <stdlib.h>

struct obj { int refs; char *name; };

void obj_destroy(struct obj *o) __attribute__((nonnull));

int f(void) {
  struct obj *o = malloc(sizeof *o);
  if (!o) return 0;
  o->refs = 1;
  o->name = NULL;
  obj_destroy(o); // UNRESOLVED: temporal:unknown-callee
  return o->refs; // UNRESOLVED: temporal:unknown-callee
}
