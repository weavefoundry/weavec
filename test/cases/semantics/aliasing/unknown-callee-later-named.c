// RFC 0030 §5.1: a call to an unknown callee may release anything reachable
// through its pointer arguments, including places the function names only
// after the call. `m->in` is first named after `consume(o)`; the object it
// points to may have been freed (it is `o->m->in`), so the access to `->v`
// must not be proven.
// STAGE: S7
struct inner { int v; };
struct mid { struct inner *in; };
struct outer { struct mid *m; };
void consume(struct outer *o);
int use_after_unknown(struct outer *o) {
  struct mid *m = o->m;
  consume(o);
  return m->in->v; // NOT-PROVEN: temporal
}
