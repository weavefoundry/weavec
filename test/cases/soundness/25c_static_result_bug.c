// Pointer to a static result buffer invalidated by the next call (not a memory error, logic) +
// getenv result used after setenv may be freed.
//
// No oracle marker: whether this is a use-after-free depends on the libc.
// Darwin's setenv releases the entry it replaces when the new value does not
// fit, and ASan reports it; glibc never frees a published "NAME=value" string
// (__add_to_environ overwrites the slot and keeps the old string), precisely
// because callers may still hold a getenv result, so on Linux ASan has nothing
// to find. The BUG marker stays: WeaveC cannot know which libc the program will
// be linked against, so it must report the read either way.
#include <stdlib.h>
int main(void) {
  setenv("WEAVEC_PROBE", "first-value", 1);
  const char *v = getenv("WEAVEC_PROBE");
  setenv("WEAVEC_PROBE", "a-much-longer-second-value-to-force-reallocation", 1);
  return v ? v[0] : 0; // BUG: use-after-free
}
