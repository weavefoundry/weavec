// RUN: %weavec --checked-function=create --checked-report=%t.json %s -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: FileCheck %s --check-prefix=REPORT < %t.json
// CLEAN-NOT: error:
// REPORT: "kind":"callback-allocate"
// RFC 0029: a generic helper exports a behavioral premise; it does not trust
// an unbound allocator's implementation.
#include <stdlib.h>
void *create(void *(*allocate)(size_t), size_t n) {
    if (!n) return 0;
    unsigned char *p = allocate(n);
    if (p) p[n - 1] = 7;
    return p;
}
