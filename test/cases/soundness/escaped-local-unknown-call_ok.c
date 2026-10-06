// RFC 0031 §5 (unknown callees), RFC 0033 §2 (the witness rule): a local
// whose address is stored in a global has escaped. Code the analysis does not
// see may keep that address when an unrelated call wipes the global, and
// write the local through it later, so a read after any unknown call is no
// use of an uninitialised value.
// UNITS: Inputs/escaped-local-unknown-call-impl.c
// CLEAN
// RUN-INPUT:
#include <string.h>
struct callbacks { void *payload; };
struct options { int version; struct callbacks callbacks; };
struct options g_options;
void clear_errors(void);
int clone_with(const struct options *options);
int main(void) {
  const char *given;
  g_options.callbacks.payload = &given;
  clear_errors();
  if (clone_with(&g_options) != 0)
    return 1;
  return strcmp(given, "redirected") == 0 ? 0 : 1;
}
