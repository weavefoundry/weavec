// RFC 0030 §11: with -fno-weavec-zero-init a possibly uninitialised pointer is unresolved(no-zero-init).
// STAGE: S5
// Without zero-initialisation the path that does not assign 'p' leaves garbage in it, which
// a null check cannot catch, so the dereference's null facet is unresolved(no-zero-init)
// instead of checked (§3.2).
// FLAGS: -fno-weavec-zero-init
int deref(int c, int *q) {
  int *p;
  if (c) p = q;
  return *p; // UNRESOLVED: null:no-zero-init
}
