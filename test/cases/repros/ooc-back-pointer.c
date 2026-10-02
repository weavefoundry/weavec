// Held-out repro (RFC 0031 Motivation, §5.6, §11.1): bzip2 bzlib.c:1301-1345
// (BZ2_bzBuffToBuffDecompress) decompresses through a stack 'bz_stream strm'. Init stores
// '&strm' into the heap state it allocates ('strm.state->strm', a back pointer); every exit
// path calls BZ2_bzDecompressEnd, which frees the state and the back pointer with it.
// v0.11.0 reports at link "'incoming(strm.state)->strm' may outlive 'strm', which it points
// to" (a false definite lifetime-too-short error at bzlib.c:1321, the Init call).
// Reduced from bzip2 1ea1ac18 into this unit (bzlib.c's caller) and the library unit
// Inputs/ooc-back-pointer-lib.c, linked together so the link step sees both records.
// intended: no finding; the cell holding '&strm' belongs to an object freed before 'strm'
// ends and no later boundary can reach it.
// UNITS: Inputs/ooc-back-pointer-lib.c
// CLEAN
// ASAN
#include <stddef.h>
#include "Inputs/ooc-back-pointer.h"
int BZ2_bzBuffToBuffDecompress(char *dest, unsigned int *destLen, char *source,
                               unsigned int sourceLen, int small, int verbosity) {
  bz_stream strm;
  int ret;
  if (dest == NULL || destLen == NULL || source == NULL || (small != 0 && small != 1) ||
      verbosity < 0 || verbosity > 4)
    return BZ_PARAM_ERROR;
  strm.bzalloc = NULL;
  strm.bzfree = NULL;
  strm.opaque = NULL;
  ret = BZ2_bzDecompressInit(&strm, verbosity, small);
  if (ret != BZ_OK) return ret;
  strm.next_in = source;
  strm.next_out = dest;
  strm.avail_in = sourceLen;
  strm.avail_out = *destLen;
  ret = BZ2_bzDecompress(&strm);
  if (ret == BZ_OK) goto output_overflow_or_eof;
  if (ret != BZ_STREAM_END) goto errhandler;
  *destLen -= strm.avail_out;
  BZ2_bzDecompressEnd(&strm);
  return BZ_OK;
output_overflow_or_eof:
  BZ2_bzDecompressEnd(&strm);
  return BZ_OUTBUFF_FULL;
errhandler:
  BZ2_bzDecompressEnd(&strm);
  return ret;
}
int main(void) {
  char src[5] = "abcd", dst[8];
  unsigned int n = sizeof dst;
  if (BZ2_bzBuffToBuffDecompress(dst, &n, src, 4, 0, 0) != BZ_OK || n != 4) return 1;
  n = 2;
  return BZ2_bzBuffToBuffDecompress(dst, &n, src, 4, 0, 0) == BZ_OUTBUFF_FULL ? 0 : 1;
}
