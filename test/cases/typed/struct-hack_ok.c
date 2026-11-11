// RFC 0042 amendment 15: the array that is a struct's last field may run
// past the struct (a flexible array member, or the struct hack `data[1]`,
// which Clang's array-bounds check leaves alone too): the access keeps its
// guard instead of a comparison with the struct's size, and a valid program
// runs.
// CLEAN
// FLAGS: -O2 -fweavec-typed
#include <weavec.h>
#include <stdlib.h>
#include <string.h>

struct message { int length; char data[1]; };

char at(const struct message *m, int i) { return m->data[i]; }

WEAVEC_GUARDED int main(void) {
  struct message *m = calloc(1, sizeof *m + 100);
  if (m == NULL)
    return 1;
  m->length = 100;
  memset(m->data, 'x', 100);
  return at(m, 50) == 'x' ? 0 : 1;
}
