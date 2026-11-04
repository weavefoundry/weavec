// RFC 0037 §5: stat's wrapper checks a whole struct stat.
// RUN-INPUT:
// ASAN
#include <stdlib.h>
#include <sys/stat.h>

int main(void) {
  struct stat *out = malloc(16);
  return stat("/", out); // TRAP: heap-buffer-overflow
}
