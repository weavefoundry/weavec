// RFC 0034 section 6.3: false definite errors the milestone found in
// libdeflate's programs/test_util.c (build/eval-2026-10-04/repros/libdeflate-1.c):
// munmap of part of a mapping is not a release of the whole mapping, so
// unmapping the two guard pages is no double free and the middle page is
// still mapped. Unconfirmed candidates may remain warnings (RFC 0034,
// Diagnostics), and so may the possible invalid release of a range that does
// not start a mapping (munmap takes any page-aligned range).
// CLEAN
// ALLOW: double-free use-after-free invalid-release
/* libdeflate programs/test_util.c alloc_guarded_buffer: munmap of part of a
 * mapping is modelled as a release of the whole mapping, so unmapping the two
 * guard pages is a "double free" and the middle page is "used after free".
 * -DNEGATIVE: unmap only the whole mapping, once, at the end -> builds, runs. */
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void) {
	size_t pg = (size_t)getpagesize();
	unsigned char *base = mmap(NULL, 3 * pg, PROT_READ | PROT_WRITE,
				   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED)
		return 1;
	unsigned char *start = base + pg;
	unsigned char *end = start + pg;
#ifndef NEGATIVE
	munmap(base, pg);   /* leading guard page only */
	munmap(end, pg);    /* trailing guard page: a different range */
#endif
	start[0] = 42;      /* middle page is still mapped */
	printf("%d\n", start[0]);
#ifndef NEGATIVE
	munmap(start, pg);
#else
	munmap(base, 3 * pg);
#endif
	return 0;
}
