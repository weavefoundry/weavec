/* Shared declarations of ooc-back-pointer.c and Inputs/ooc-back-pointer-lib.c (bzlib.h and
   bzlib_private.h, reduced). */
#define BZ_OK 0
#define BZ_STREAM_END 4
#define BZ_PARAM_ERROR (-2)
#define BZ_MEM_ERROR (-3)
#define BZ_OUTBUFF_FULL (-8)
typedef struct {
  char *next_in;
  unsigned int avail_in;
  char *next_out;
  unsigned int avail_out;
  void *state;
  void *(*bzalloc)(void *, int, int);
  void (*bzfree)(void *, void *);
  void *opaque;
} bz_stream;
typedef struct {
  bz_stream *strm;
  int state;
  unsigned int *tt;
  int verbosity;
} DState;
int BZ2_bzDecompressInit(bz_stream *strm, int verbosity, int small);
int BZ2_bzDecompress(bz_stream *strm);
int BZ2_bzDecompressEnd(bz_stream *strm);
