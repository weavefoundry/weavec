// Held-out repro (RFC 0031 Motivation, §9.1, §11.1): miniz miniz_zip.c:2911
// (mz_zip_file_write_func) writes 'n' bytes of 'pBuf' with 'MZ_FWRITE(pBuf, 1, n, file)';
// when an archive entry is empty, 'pBuf' is NULL and 'n' is 0 (miniz's example programs hit
// it). fwrite with a zero count reads no bytes of its buffer.
// v0.11.0's library table requires fwrite's buffer to be non-null whatever the count, so
// the buffer's null facet is checked and the correct program traps (nonnull) at run time.
// intended: no finding and no trap; fwrite's buffer is non-null only when the count is
// non-zero (the zero-length form of the library row).
// RUN-INPUT:
// RUN-INPUT: 1
// CLEAN
// ASAN
#include <stdio.h>
#include <stdlib.h>
struct entry { const char *data; size_t size; };
static size_t write_func(FILE *file, const void *pBuf, size_t n) {
  return fwrite(pBuf, 1, n, file);
}
int main(int argc, char **argv) {
  (void)argv;
  struct entry e = { NULL, 0 };
  if (argc > 1) { e.data = "payload"; e.size = 7; }
  FILE *f = tmpfile();
  if (!f) return 0;
  size_t written = write_func(f, e.data, e.size);
  fclose(f);
  return written == e.size ? 0 : 1;
}
