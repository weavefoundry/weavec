/* Error helpers that never return, unannotated: their `never-returns` is
 * inferred here and crosses the unit boundary (RFC 0009, RFC 0005). */
#include "../../Inputs/prelude.h"

void abort(void);

void die(const char *msg) {
  use(msg);
  abort();
}

void fail(int code) {
  if (code > 3)
    die("big");
  die("small");
}

/* Returns when `ok` is non-zero: not `never-returns`. */
void check(int ok) {
  if (!ok)
    die("bad");
}
