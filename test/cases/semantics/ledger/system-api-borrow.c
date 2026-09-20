// RFC 0030 §5.2: a platform function without a table entry borrows its arguments; later uses are unaffected.
// STAGE: S3
// 'ioctl' is declared in a platform header and has no LibrarySpec entry: the call borrows
// 'ws' with no release, retain or store effect, and its temporal facet is
// trusted(system-api). The later read and the release of 'ws' are proven under A2, so no
// temporal facet of the unit is unresolved.
// CLEAN
// EXPECT-LEDGER: /summary/facets/temporal/unresolved == 0
#include <stdlib.h>
#include <sys/ioctl.h>

int rows(int fd) {
  struct winsize *ws = malloc(sizeof *ws);
  if (!ws) return 24;
  int r = 24;
  if (ioctl(fd, TIOCGWINSZ, ws) != -1) // TRUSTED: temporal:system-api
    r = ws->ws_row;
  free(ws);
  return r;
}
