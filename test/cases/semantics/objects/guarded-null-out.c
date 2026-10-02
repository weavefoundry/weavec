// RFC 0031 §6.1: a null stored through an optional output.
// STAGE: S7
// `compile` stores null through `errorp` when it succeeds, and the error
// when it fails, each under `if (errorp)` (mujs's `regcompx`). The join of
// "stored null" and "left the entry value" is the entry value or null,
// which the summary described as the cell's own value and so as no store:
// the caller's `error` stayed unassigned, a definite (and false)
// `use-of-uninitialized` where it read it after a failure.
// CLEAN
// ASAN
// RUN-INPUT:
#include <stdlib.h>

static void *compile(const char *pattern, const char **errorp) {
  if (!pattern || !*pattern) {
    if (errorp)
      *errorp = "empty pattern";
    return NULL;
  }
  void *prog = malloc(8);
  if (!prog)
    return NULL;
  if (errorp)
    *errorp = NULL;
  return prog;
}

int main(void) {
  const char *error = "unset";
  void *prog = compile("", &error);
  if (prog || error[0] != 'e')
    return 1;
  const char *other;
  prog = compile("a", &other);
  if (!prog)
    return 1;
  int ok = other == NULL;
  free(prog);
  free(compile("b", NULL));
  return ok ? 0 : 1;
}
