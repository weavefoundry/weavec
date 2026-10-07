// RFC 0032, Accepted false positives: an interior pointer and a negative index stay inside the block: the header idiom.
// STAGE: S3
// The string is handed out as a pointer past its length header, and the header is read
// back at a negative offset.
// CLEAN
// RUN-INPUT:
// ASAN
#include <stdlib.h>
#include <string.h>
struct hdr { size_t len; };
static char *str_new(const char *init) {
  size_t n = strlen(init);
  struct hdr *h = malloc(sizeof *h + n + 1);
  if (!h) return NULL;
  h->len = n;
  memcpy(h + 1, init, n + 1);
  return (char *)(h + 1);
}
static size_t str_len(const char *s) {
  return ((const struct hdr *)s)[-1].len;
}
static void str_free(char *s) {
  free((struct hdr *)s - 1);
}
int main(void) {
  char *s = str_new("hello");
  if (!s) return 1;
  int r = str_len(s) != 5;
  str_free(s);
  return r;
}
