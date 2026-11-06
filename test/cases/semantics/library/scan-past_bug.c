// RFC 0038 §2: a search that reads one element past its heap block stops,
// at the call: past the bytes up to where it stopped (a count, a match),
// or, when a search for a pointer found nothing, as an unterminated string. Each run reads past with one row.
// (glibc's rawmemchr, which Darwin lacks, is in test/Guards/library-scan.c.)
// RUN-INPUT: strcspn
// RUN-INPUT: strspn
// RUN-INPUT: strchr
// RUN-INPUT: strchrnul
// RUN-INPUT: strpbrk
// RUN-INPUT: strstr
// RUN-INPUT: memchr
// RUN-INPUT: wcscspn
// RUN-INPUT: wcschr
// RUN-INPUT: wmemchr
#define _GNU_SOURCE 1
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* A block that holds no terminator and no dot, so a search for either
 * reads past it (to the zero bytes after its end). */
static char *block(void) {
  char *p = malloc(15);
  memcpy(p, "wordwordwordwor", 15);
  return p;
}

static wchar_t *wide(void) {
  wchar_t *p = malloc(15 * sizeof(wchar_t));
  wmemcpy(p, L"wordwordwordwor", 15);
  return p;
}

int main(int argc, char **argv) {
  char *p = block();
  wchar_t *w = wide();
  const char *name = argc > 1 ? argv[1] : "";
  if (strcmp(name, "strcspn") == 0)
    return (int)strcspn(p, "."); // TRAP: heap-buffer-overflow
  if (strcmp(name, "strspn") == 0)
    return (int)strspn(p, "word"); // TRAP: heap-buffer-overflow
  if (strcmp(name, "strchr") == 0)
    return strchr(p, '.') != NULL; // TRAP: unterminated-string
  if (strcmp(name, "strchrnul") == 0)
    return *strchrnul(p, '.'); // TRAP: heap-buffer-overflow
  if (strcmp(name, "strpbrk") == 0)
    return strpbrk(p, ".") != NULL; // TRAP: unterminated-string
  if (strcmp(name, "strstr") == 0)
    return strstr(p, "r.") != NULL; // TRAP: unterminated-string
  if (strcmp(name, "memchr") == 0)
    return memchr(p, '.', 16) != NULL; // TRAP: heap-buffer-overflow
  if (strcmp(name, "wcscspn") == 0)
    return (int)wcscspn(w, L"."); // TRAP: heap-buffer-overflow
  if (strcmp(name, "wcschr") == 0)
    return wcschr(w, L'.') != NULL; // TRAP: unterminated-string
  if (strcmp(name, "wmemchr") == 0)
    return wmemchr(w, L'.', 16) != NULL; // TRAP: heap-buffer-overflow
  return 0;
}
