// RFC 0030, section 10.6, gate G8: an index checked against a counted field that is read
// through a pointer not known to be non-null: the term checks that pointer itself before it
// reads the count (section 10.3 rule 5).
// The -O0 IR equals that of Inputs/rewrite-oracle-index-counted-field.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-index-counted-field.expected.c %t

#include <weavec.h>
struct buf { char *WEAVEC_COUNTED_BY(cap) data; unsigned long cap; };
char at(const struct buf *b, unsigned long i) { return b->data[i]; }
