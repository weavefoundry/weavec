// RFC 0030, section 10.6, gate G8: the zero-length form: memcpy's pointers may be null when the length is 0 (§8.3).
// The two parameters may overlap, so the destination also gets memcpy's
// overlap check, around its `nonnull` check (section 10.4: `nonnull` is
// innermost).
// The -O0 IR equals that of Inputs/rewrite-oracle-nonnull-zero-length.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-nonnull-zero-length.expected.c %t

void *memcpy(void *, const void *, unsigned long);
void copy(char *d, const char *s, unsigned long n) { memcpy(d, s, n); }
