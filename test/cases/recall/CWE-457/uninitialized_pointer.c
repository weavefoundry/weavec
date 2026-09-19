// CWE-457: use of an uninitialised pointer variable.
#include "../recall.h"

void bad(void) {
  char *data;
  data[0] = 'A'; // BUG: use-of-uninitialized // TRAP: nonnull
}

void bad_on_path(int flag) {
  char buf[10];
  char *data;
  if (flag)
    data = buf;
  print_line(data); // BUG: use-of-uninitialized
}

void good(int flag) {
  char *data = NULL;
  if (flag)
    data = malloc(10);
  if (!data)
    return;
  data[0] = 'A';
  free(data);
}

// bad_on_path only passes the pointer on; zero-initialisation makes that harmless, so no run executes it.
// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
void print_line(const char *s) { (void)s; }
int main(void) {
  good(1);
  bad();
  return 0;
}
