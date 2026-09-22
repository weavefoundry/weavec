// RFC 0030 §4 "Require levels": under -fweavec-require=checked, unresolved facets are errors.
// STAGE: S3
// The unknown-extent example fails with "access 'b[1000]' is neither proven nor checkable:
// the extent of 'b' is unknown [unknown-extent] [weavec::unresolved-operation]". The call's
// temporal facet is unresolved(unknown-callee) too, which the §6.3 table makes a second
// unresolved-operation error. The checked null facet of b[1000] is allowed at this level.
// FLAGS: -fweavec-require=checked
// EXPECT-LEDGER: /summary/errors == 2
char *get_buffer(void); /* no definition, entry or annotation */

int f(void) {
  char *b = get_buffer(); // BUG: unresolved-operation definite
  return b[1000]; // BUG: unresolved-operation definite
}
