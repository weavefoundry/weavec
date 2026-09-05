// CWE-121: stack-based buffer overflow through fgets with a size larger than
// the buffer.
#include "recall.h"

void bad(void *stream) {
  char line[64];
  if (fgets(line, 128, stream)) // RECALL: out-of-bounds
    print_line(line);
}

void good(void *stream) {
  char line[64];
  if (fgets(line, 64, stream))
    print_line(line);
}
