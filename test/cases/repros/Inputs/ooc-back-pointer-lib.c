// Library unit of ooc-back-pointer.c: bzip2 bzlib.c's BZ2_bzDecompressInit and
// BZ2_bzDecompressEnd, reduced. Init allocates the private state through the stream's
// allocator hooks and links the two both ways ('s->strm = strm; strm->state = s'); End
// frees the state, and the back pointer with it, and clears 'strm->state'.
#include <stdlib.h>
#include "ooc-back-pointer.h"
static void *default_bzalloc(void *opaque, int items, int size) { (void)opaque; return malloc((size_t)items * (size_t)size); }
static void default_bzfree(void *opaque, void *addr) { (void)opaque; if (addr != NULL) free(addr); }
#define BZALLOC(nnn) (strm->bzalloc)(strm->opaque, (nnn), 1)
#define BZFREE(ppp) (strm->bzfree)(strm->opaque, (ppp))
int BZ2_bzDecompressInit(bz_stream *strm, int verbosity, int small) {
  DState *s;
  if (strm == NULL) return BZ_PARAM_ERROR;
  if (small != 0 && small != 1) return BZ_PARAM_ERROR;
  if (verbosity < 0 || verbosity > 4) return BZ_PARAM_ERROR;
  if (strm->bzalloc == NULL) strm->bzalloc = default_bzalloc;
  if (strm->bzfree == NULL) strm->bzfree = default_bzfree;
  s = BZALLOC((int)sizeof(DState));
  if (s == NULL) return BZ_MEM_ERROR;
  s->strm = strm;
  strm->state = s;
  s->state = 10;
  s->tt = NULL;
  s->verbosity = verbosity;
  return BZ_OK;
}
int BZ2_bzDecompress(bz_stream *strm) {
  DState *s;
  if (strm == NULL) return BZ_PARAM_ERROR;
  s = strm->state;
  if (s == NULL) return BZ_PARAM_ERROR;
  if (s->strm != strm) return BZ_PARAM_ERROR;
  while (strm->avail_in > 0 && strm->avail_out > 0) {
    *strm->next_out++ = *strm->next_in++;
    strm->avail_in--;
    strm->avail_out--;
  }
  return strm->avail_in == 0 ? BZ_STREAM_END : BZ_OK;
}
int BZ2_bzDecompressEnd(bz_stream *strm) {
  DState *s;
  if (strm == NULL) return BZ_PARAM_ERROR;
  s = strm->state;
  if (s == NULL) return BZ_PARAM_ERROR;
  if (s->strm != strm) return BZ_PARAM_ERROR;
  if (s->tt != NULL) BZFREE(s->tt);
  BZFREE(strm->state);
  strm->state = NULL;
  return BZ_OK;
}
