// RFC 0041 §6, amendment 19: an array of variable length whose address
// escapes is placed on the heap. A path that never reaches it releases
// nothing, and a loop's next iteration releases the block before it
// allocates another, so the loop does not leak.
// CLEAN
// FLAGS: -O2
#include <weavec.h>

static char *kept;

WEAVEC_SAFE void keep(char *WEAVEC_ESCAPES p) { kept = p; }

WEAVEC_SAFE int fill(int n) {
  int total = 0;
  if (n > 0) {
    char v[n];
    v[0] = 1;
    v[n - 1] = 2;
    keep(v);
    total = v[0] + v[n - 1];
  }
  return total;
}

WEAVEC_SAFE int loop(int count, int n) {
  int total = 0;
  for (int i = 0; i < count; i++) {
    char v[n];
    v[0] = (char)i;
    keep(v);
    total += v[0] & 1;
  }
  return total;
}

int main(void) {
  int total = 0;
  for (int i = 0; i < 1000; i++)
    total += fill(i & 1 ? 64 : 0);
  // 1 GiB if each iteration's 256 KiB block were kept.
  total += loop(4096, 256 * 1024);
  kept = 0;
  return total == 3 * 500 + 2048 ? 0 : 1;
}
