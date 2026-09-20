// CWE-121: stack-based buffer overflow through fgets with a size larger than
// the buffer.
#include "../recall.h"

void bad(void *stream) {
  char line[64];
  if (fgets(line, 128, stream)) // BUG: out-of-bounds // TRAP: len
    print_line(line);
}

void good(void *stream) {
  char line[64];
  if (fgets(line, 64, stream))
    print_line(line);
}

// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
void *fopen(const char *path, const char *mode);
int fclose(void *stream);
void print_line(const char *s) { (void)s; }
int main(void) {
  void *zero = fopen("/dev/zero", "r");
  if (!zero)
    return 1;
  good(zero);
  bad(zero);
  fclose(zero);
  return 0;
}
