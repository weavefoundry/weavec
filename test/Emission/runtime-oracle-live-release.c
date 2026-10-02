// RFC 0032, sections 3 and 6: a dereference whose object may have been released by a call
// the analysis cannot see is guarded by `live`, outside its null check; a release of a
// pointer that is not known to be the start of a live heap object is guarded by `release`.
// The -O0 IR equals that of Inputs/runtime-oracle-live-release.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/runtime-oracle-live-release.expected.c %t -- -fweavec-runtime -fno-weavec-global-objects -fno-weavec-zero-init -Wno-weavec

struct vec {
  int *data;
  unsigned long n;
};
extern void unknown(void);
extern void free(void *);
extern int *G;

int after(void) {
  int *p = G;
  unknown();
  return *p;
}

void drop(struct vec *v) { free(v->data); }
