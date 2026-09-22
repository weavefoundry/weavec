// RFC 0030 §7.4: '&ts->contents' has the extent of the whole allocation (Lua's short strings).
// STAGE: S3
// A pointer made by '&member' has the extent of the complete object, as
// __builtin_object_size mode 0 does, so the memcpy of 'l' bytes into (char *)&ts->contents
// and the terminator store after it are in bounds: the allocation holds
// offsetof(TString, contents) + l + 1 bytes. memcpy is not a str* row, so the member's own
// bound (one char) is not used. No error, no trap.
// CLEAN
// ASAN
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct TString {
  unsigned char tt;
  unsigned char shrlen;
  unsigned hash;
  char contents[1];
} TString;

static TString *newshrstr(const char *str, size_t l) {
  TString *ts = malloc(offsetof(TString, contents) + l + 1);
  if (ts == NULL) return NULL;
  ts->tt = 4;
  ts->shrlen = (unsigned char)l;
  ts->hash = 0;
  memcpy((char *)&ts->contents, str, l);
  ((char *)&ts->contents)[l] = '\0';
  return ts;
}

int main(void) {
  TString *ts = newshrstr("hello", 5);
  if (ts == NULL) return 1;
  int ok = strcmp((const char *)&ts->contents, "hello") == 0;
  free(ts);
  return ok ? 0 : 1;
}
