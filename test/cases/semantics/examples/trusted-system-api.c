// RFC 0030 §4 "Trusted, system API": ioctl has no table entry, so its call is trusted.
// STAGE: S3
// The layout of ioctl's third argument depends on the request, so it has no LibrarySpec
// entry. Declared in a platform header, it borrows its arguments for the call (the address
// of 'ws' is borrowed) and its Call site's temporal facet is trusted(system-api) (§5.2).
// CLEAN
#include <sys/ioctl.h>

int cols(int fd) {
  struct winsize ws;
  if (ioctl(fd, TIOCGWINSZ, &ws) == -1) // TRUSTED: temporal:system-api
    return 80;
  return ws.ws_col;
}
