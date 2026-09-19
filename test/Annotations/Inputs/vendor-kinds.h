/* A third-party header found via -isystem: its attributes are §7.2 level 4
 * (RFC 0030). */
#ifndef WEAVEC_TEST_VENDOR_KINDS_H
#define WEAVEC_TEST_VENDOR_KINDS_H

void vendor_fill(int *p) __attribute__((nonnull));
void *vendor_alloc(unsigned long n) __attribute__((malloc));

#endif /* WEAVEC_TEST_VENDOR_KINDS_H */
