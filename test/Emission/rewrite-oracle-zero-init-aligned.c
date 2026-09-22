// RFC 0030, section 10.6, gate G8: an allocator without a same-signature wrapper is zeroed through zero_tail.
// The -O0 IR equals that of Inputs/rewrite-oracle-zero-init-aligned.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-zero-init-aligned.expected.c %t

void *aligned_alloc(unsigned long, unsigned long);
void *get(unsigned long n) { return aligned_alloc(16, n); }
