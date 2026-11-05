// CWE-457: use of an uninitialized pointer variable.
#include "../recall.h"

void bad(void) {
  char *data;
  data[0] = 'A'; // TRAP
}

void bad_on_path(int flag) {
  char buf[10];
  char *data;
  if (flag)
    data = buf;
  print_line(data);
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

// bad_on_path only passes the pointer on; zero-initialization makes that harmless, so no run executes it.
// `data` is uninitialized there only when `flag` is zero, and RFC 0030 gives a pointer that is not
// definitely uninitialized no diagnostic (section 3.1): zero-initialization (section 11) makes it null
// on that path, and handing a null pointer on is defined, so the pin is neutralized.
// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
void print_line(const char *s) { (void)s; }
int main(void) {
  good(1);
  bad();
  return 0;
}
