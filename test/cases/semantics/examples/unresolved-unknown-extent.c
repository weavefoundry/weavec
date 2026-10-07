// RFC 0030 §4 "Unresolved, unknown extent": no check is written against a lower bound.
// STAGE: S3
// 'b' is Single-or-nullable under A3 (§7.3). Single is a lower bound that covers element 0
// only, so b[1000] has spatial unresolved(unknown-extent), null checked (nonnull) and
// temporal proven. The call is a Call site whose temporal facet is unresolved(unknown-callee)
// and carries a fix-it suggestion in the ledger (site 0 of 'f').
char *get_buffer(void); /* no definition, entry or annotation */

int f(void) {
  char *b = get_buffer();
  return b[1000]; // TRAP
}
