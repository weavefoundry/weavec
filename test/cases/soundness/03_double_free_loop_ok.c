// CLEAN
// The `break` is reachable only with i > 5, which takes six completed turns of
// the loop, each of which frees 'p' and sets it to null; 'p' is therefore null
// wherever the break is taken. The loop head joins the state before the first
// turn (p owned, i == 0) with the state after one (p null, i >= 1) and keeps
// no correlation between the counter and that join, so under the guard i > 5
// 'p' still reads as may-owned and the exit reports a possible leak. The fix
// is a loop-head state that distinguishes the first turn from the later ones,
// which RFC 0030 does not have; until then the warning is tolerated here.
// ALLOW: leak
// ASAN
#include <stdlib.h>
int main(int argc, char **argv) {
  (void)argv;
  char *p = malloc(8);
  if (!p) return 1;
  for (int i = 0; i < argc + 1; i++) {
    if (i > 5) break;
    free(p);
    p = NULL;
  }
  return 0;
}
