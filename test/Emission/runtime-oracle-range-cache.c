// RFC 0032, section 13: a guard in a loop gets a range cache in its function's frame, four
// words that are cleared on entry, and compares its pointer with the cache before it asks
// the runtime. A loop that calls nothing checks the cache's state once on the way in, and
// its guards do not read it (their last argument is 1); a loop with a call reads the state
// at every guard.
// The -O0 IR equals that of Inputs/runtime-oracle-range-cache.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/runtime-oracle-range-cache.expected.c %t -- -fweavec-runtime -fno-weavec-global-objects -Wno-weavec

struct vec {
  int *data;
  unsigned long n;
};
extern void unknown(void);

int quiet(struct vec *v) {
  int s = 0;
  for (unsigned long i = 0; i < v->n; i++)
    s += v->data[i];
  return s;
}

int calls(struct vec *v) {
  int s = 0;
  for (unsigned long i = 0; i < v->n; i++) {
    s += v->data[i];
    unknown();
  }
  return s;
}
