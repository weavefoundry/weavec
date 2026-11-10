// RFC 0041 amendment 15: a negative signed count is an empty range, so a
// function that handles it itself runs.
// CLEAN
// FLAGS: -O1
#include <weavec.h>
#include <sys/types.h>

WEAVEC_SAFE int commas(const char *WEAVEC_COUNTED_BY(len) s, ssize_t len) {
  if (len <= 0)
    return 0;
  int n = 0;
  for (ssize_t i = 0; i < len; i++)
    n += s[i] == ',';
  return n;
}

int main(void) { return commas("a,b", -1) + commas("a,b", 3) == 1 ? 0 : 1; }
