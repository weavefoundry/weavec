// RFC 0038 §2: every search row of the library table, called on a heap
// block of exactly the extent the row states, with no terminator where the
// call stops before one: none stops (git's fsck calls strcspn so).
// CLEAN
// RUN-INPUT:
#define _GNU_SOURCE 1
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>

/* `n` bytes of `text`, in a block of exactly that size. */
static char *block(const char *text, size_t n) {
  char *p = malloc(n);
  memcpy(p, text, n);
  return p;
}

static wchar_t *wide(const wchar_t *text, size_t n) {
  wchar_t *p = malloc(n * sizeof(wchar_t));
  memcpy(p, text, n * sizeof(wchar_t));
  return p;
}

static volatile int sink;

int main(void) {
  char *tree = block("tree 1234\nparent", 16);
  char *upper = block("TREE 1234\nparent", 16);
  wchar_t *wtree = wide(L"tree 1234\nparent", 16);
  sink += (int)strcspn(tree, "<>\n");
  sink += (int)strspn(tree, "eert");
  sink += strchr(tree, '\n') != NULL;
  sink += index(tree, '\n') != NULL;
  sink += strchrnul(tree, '\n') != NULL;
  sink += strpbrk(tree, "\n") != NULL;
  sink += strstr(tree, "1234") != NULL;
  sink += strcasestr(upper, "1234") != NULL;
  sink += memchr(tree, '\n', 1000) != NULL;
#if defined(__GLIBC__)
  sink += rawmemchr(tree, '\n') != NULL;
#endif
  sink += (int)wcscspn(wtree, L"\n");
  sink += (int)wcsspn(wtree, L"eert");
  sink += wcschr(wtree, L'\n') != NULL;
  sink += wcspbrk(wtree, L"\n") != NULL;
  sink += wcsstr(wtree, L"1234") != NULL;
  sink += wmemchr(wtree, L'\n', 1000) != NULL;
  /* A search that finds nothing reads its whole string, terminator
   * included, or its whole count. */
  {
    char *word = block("word", 5);
    wchar_t *wword = wide(L"word", 5);
    sink += strchr(word, 'z') == NULL;
    sink += strpbrk(word, "z") == NULL;
    sink += strstr(word, "zz") == NULL;
    sink += memchr(word, 'z', 5) == NULL;
    sink += wcschr(wword, L'z') == NULL;
    sink += wmemchr(wword, L'z', 5) == NULL;
    /* A count of zero reads nothing, even at a block's end. */
    sink += memchr(word + 5, 'z', 0) == NULL;
  }
  return sink == 0;
}
