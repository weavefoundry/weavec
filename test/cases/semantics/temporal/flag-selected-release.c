// RFC 0030 §3.4: what a release is of is as certain as the release itself.
// STAGE: S8
// `wrap` releases `text` only when `own` is set; the join of the two arms
// keeps the alias between `v` and `text` without the flag, so the release of
// the argument is claimed on paths that do not perform it. It may therefore be
// reported, but never as a definite finding: `make_literal` passes a string
// literal, and a release the callee may not perform makes that a possible
// `invalid-release`, not an error that would drop the object.
// CLEAN
// ALLOW: invalid-release
#include <stdlib.h>
#include <string.h>

struct text {
  char *bytes;
};

static struct text *wrap(char *text, int own) {
  struct text *t;
  char *v;
  if (!text)
    return NULL;
  if (own)
    v = text;
  else {
    v = strdup(text);
    if (!v)
      return NULL;
  }
  t = malloc(sizeof *t);
  if (!t) {
    free(v);
    return NULL;
  }
  t->bytes = v;
  return t;
}

static void release(struct text *t) {
  if (t)
    free(t->bytes);
  free(t);
}

int main(void) {
  struct text *copy = wrap("abc", 0);
  char *owned = strdup("xyz");
  struct text *moved = NULL;
  if (!owned) {
    release(copy);
    return 1;
  }
  // `wrap` takes the buffer when `own` is set, and releases it itself on the
  // one path where it then fails, so there is nothing left to free here.
  moved = wrap(owned, 1);
  release(copy);
  release(moved);
  return 0;
}
