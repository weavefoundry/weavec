// Salvaged false proof (RFC 0030 section 17.2): rfc0029/readers, case reader-short. See SOURCES.md.
// consume() reads every position up to end, but data has two bytes and end is 4.
// UNITS: Inputs/reader-short-cursor.c
#include "Inputs/cursor.h"
int main(void) {
  const unsigned char data[] = {1, 2};
  struct cursor c = {0, 0, data, 4, 0};
  return (int)consume(&c);
}
