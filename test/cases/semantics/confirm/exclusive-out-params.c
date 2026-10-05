// RFC 0034 section 6.3 (exclusive stores of one fresh object): a false
// definite double free the milestone found in curl's lib/setopt.c
// (build/eval-2026-10-04/repros/curl-1.c): parse() stores a fresh
// allocation through *a on one path and through *b on the other, so a and b
// never hold the same object. Each argument count takes another path. An
// unconfirmed candidate may remain a warning (RFC 0034, Diagnostics).
// CLEAN
// ALLOW: double-free
// RUN-INPUT:
// RUN-INPUT: x
// RUN-INPUT: x y
/* WeaveC false positive: error "'b' is freed twice [weavec::double-free]".
 * Reduced from curl lib/setopt.c:180 (setstropt_interface), reported only in
 * the curlu unity build where Curl_parse_interface's body (cf-socket.c) is in
 * the same TU. parse() writes a fresh allocation to *a on one path and to *b
 * on the other; the derived summary appears to give both out-params the same
 * abstract object, so take(a) freeing a "frees" b too. a and b never alias.
 * Correct C: clean under clang -fsanitize=address,undefined for argc 1, 2, 3.
 * Negatives (each removes the error): parse() writes both *a and *b
 * unconditionally; strdup inlined into main; take() never frees; adding
 * `if (a == b) return 9;` after parse(); parse() only declared (body in
 * another unit). */
#include <stdlib.h>
#include <string.h>

static char *slot;

/* Takes ownership of str: stores it, or frees it on failure. */
static int take(char *str, int fail) {
  if (fail) { free(str); return 1; }
  free(slot);
  slot = str;
  return 0;
}

static void parse(int which, char **a, char **b) {
  if (which) *a = strdup("a");
  else       *b = strdup("b");
}

int main(int argc, char **argv) {
  char *a = NULL, *b = NULL;
  int r;
  (void)argv;
  parse(argc > 1, &a, &b);
  r = take(a, argc > 2);   /* a: stored or freed */
  if (!r) {
    r = take(b, 0);
    b = NULL;
  }
  free(b);                 /* b still owned here only if take(a) failed */
  free(slot);
  return 0;
}
