// RFC 0032, sections 3 and 7: an element of a pointer whose extent is unknown is guarded in
// place: `object` takes the base, the index, the element's size, the offset and the width of
// the access, and answers the element's address.
// The -O0 IR equals that of Inputs/runtime-oracle-object-index.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/runtime-oracle-object-index.expected.c %t -- -fweavec-runtime -fno-weavec-global-objects

struct vec {
  int *data;
  unsigned long n;
};

int at(struct vec *v, unsigned long i) { return v->data[i]; }
void put(struct vec *v, unsigned long i, int x) { v->data[i] = x; }
