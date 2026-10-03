// RFC 0032, section 6: a library call's argument whose extent is unknown is guarded where it
// is passed: `object_n` with the bytes the call needs, `object_s` for a string.
// The -O0 IR equals that of Inputs/runtime-oracle-object-call.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/runtime-oracle-object-call.expected.c %t -- -fweavec-runtime -fno-weavec-global-objects -Wno-weavec

struct vec {
  int *data;
  unsigned long n;
  char *name;
};
extern void *memset(void *, int, unsigned long);
extern unsigned long strlen(const char *);

void clear(struct vec *v, unsigned long n) { memset(v->data, 0, n); }
unsigned long len(struct vec *v) { return strlen(v->name); }
