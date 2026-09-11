// RFC 0022: canonical type identity must not discard stronger typedef
// alignment.
typedef int Overaligned __attribute__((aligned(32)));
int main(void) {
  _Alignas(32) int values[2] = {0, 7};
  void *erased = &values[1];
  Overaligned *restored = erased;
  return *restored;
}
