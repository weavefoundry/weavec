// Salvaged false proof (RFC 0030 section 17.2): rfc0029/readers, case reader-uninitialized. See SOURCES.md.
// consume() reads data[1..3], which are never written; zero-initialisation defines those reads.
// UNITS: Inputs/reader-uninitialized-cursor.c
#include "Inputs/cursor.h"
int main(void) {
  unsigned char data[4]; data[0] = 1;
  struct cursor c = {0, 0, data, sizeof data, 0};
  return (int)consume(&c);
}
