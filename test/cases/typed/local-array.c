// RFC 0042 §4: a local array's index is checked against its declared length
// ((B)) before any comparison with the local's bounds.
// FLAGS: -O2 -fweavec-typed
#include <weavec.h>
#include <stddef.h>

int pick(size_t i) {
  int table[4] = {1, 2, 3, 4};
  return table[i]; // TRAP: index-out-of-bounds
}

WEAVEC_GUARDED int main(int argc, char **argv) {
  (void)argv;
  return pick((size_t)argc + 3);
}
