// RFC 0022: ordinary compatible typedef views retain supported alignment.
typedef int Overaligned;
int main(void) {
  _Alignas(32) int values[2] = {0, 7};
  void *erased = &values[1];
  Overaligned *restored = erased;
  return *restored;
}
