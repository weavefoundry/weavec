// RFC 0018 fixed checked-code evaluation: recorded unsafe trust.
#include <stdint.h>
#include <weavec.h>
WEAVEC_CHECKED int access(uintptr_t p) { WEAVEC_UNSAFE { return *(int *)p; } }
