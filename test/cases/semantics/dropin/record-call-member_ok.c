// RFC 0033 §1: a member of a record a call returns (`parse(s).name`) is read
// from the call's temporary, which the callee's summary or an unknown callee
// filled, not from a copy that holds only its first word (libxml2's
// `htmlParseHTMLName(ctxt, 0).name`: a false use-of-uninitialized error).
// STAGE: S8
// CLEAN
// UNITS: Inputs/record-lookup.c
// RUN-INPUT:
#include <stdlib.h>
typedef struct { unsigned hashValue; const char *name; } hashed;
hashed lookup(const char *s, int n);
static hashed parse(const char *s) {
  hashed ret;
  ret = lookup(s, 3);
  if (ret.name == NULL) abort();
  return ret;
}
int main(void) {
  const char *name = parse("abc").name;
  if (name == NULL) return 1;
  return name[0] == 'a' ? 0 : 1;
}
