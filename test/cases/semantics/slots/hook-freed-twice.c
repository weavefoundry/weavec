// RFC 0030 §9.3: a release through a hook slot whose every function releases.
// STAGE: S7
// `hooks.deallocate` holds `internal_free` or `free`; either releases the
// string, so calling it twice on the same string is a double free (cJSON's
// `global_hooks.deallocate`).
// TOOL
#include <stdlib.h>

typedef struct {
  void (*deallocate)(void *);
} hooks_t;

static void internal_free(void *p) { free(p); }

static hooks_t hooks = {internal_free};

void use_plain_free(void) { hooks.deallocate = free; }

typedef struct {
  char *text;
} item;

void destroy(item *it) {
  if (it->text != NULL) {
    hooks.deallocate(it->text);
    hooks.deallocate(it->text); // BUG: double-free
    it->text = NULL;
  }
}
