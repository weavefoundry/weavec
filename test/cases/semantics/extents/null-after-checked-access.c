// RFC 0030 §3.2: after a checked access the pointer is not null, in every
// block after it.
// STAGE: S7
// `s[-1]` checks `s` against null (the check traps on null); the header read
// in the `switch` case that follows, in another block, is then proven, as sds's
// `sdslen` is. The refinement is made in every pass of the analysis, so the
// blocks after the access start from it.
// CLEAN
// TOOL
#include <stddef.h>

struct hdr8 {
  unsigned char len;
  unsigned char alloc;
  unsigned char flags;
};

size_t length(const char *s) {
  unsigned char flags = (unsigned char)s[-1];
  switch (flags & 7) {
  case 1:
    return ((const struct hdr8 *)(s - sizeof(struct hdr8)))->len;
  default:
    return 0;
  }
}
