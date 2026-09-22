// CWE-476: NULL pointer dereference, an allocation used without a check and
// a pointer set to NULL then dereferenced.
// RFC 0030 §8.4: an allocation result used without a null test is the
// allocation-failure id (off by default) and a checked null facet.
// FLAGS: -Wweavec-allocation-failure
// RUN-INPUT: 1
// RUN-INPUT: 2
#include "../recall.h"

void bad_unchecked_malloc(void) {
  char *data = malloc(100);
  data[0] = 'A'; // BUG: allocation-failure
  free(data);
}

void bad_assigned_null(void) {
  int *p = NULL;
  *p = 1; // BUG: null-dereference // TRAP: nonnull
}

void bad_after_test(int *p) {
  if (p == NULL)
    print_int(*p); // BUG: null-dereference // TRAP: nonnull
}

void good(void) {
  char *data = malloc(100);
  if (!data)
    return;
  data[0] = 'A';
  free(data);
}

// bad_unchecked_malloc misbehaves only when malloc fails, so no run executes it.
// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
void print_int(int value) { (void)value; }
int main(int argc, char **argv) {
  int which = argc > 1 ? argv[1][0] - '0' : 0;
  good();
  if (which == 1) bad_assigned_null();
  if (which == 2) bad_after_test(NULL);
  return 0;
}
