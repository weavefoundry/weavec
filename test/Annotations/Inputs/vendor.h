/* Stands in for a third-party header found via -isystem. Calls into it are
 * not reported as unchecked boundaries (RFC 0003). */
#ifndef WEAVEC_TEST_VENDOR_H
#define WEAVEC_TEST_VENDOR_H

void vendor_touch(void *p);

#endif /* WEAVEC_TEST_VENDOR_H */
