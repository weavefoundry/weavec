// strtok keeps a pointer into a buffer that is freed before the next call.
// The first token is longer than the 16 bytes a released block keeps its
// release record in (RFC 0040, amendment 4), so the next call resumes in the
// string's own bytes and returns a pointer into the freed block, whatever
// the record holds.
#include <stdlib.h>
#include <string.h>
int main(void) {
  char *s = strdup("aaaaaaaaaaaaaaaaaaaa,b,c");
  if (!s) return 1;
  char *t = strtok(s, ",");
  free(s);
  t = strtok(NULL, ",");
  return t ? t[0] : 0; // TRAP
}
