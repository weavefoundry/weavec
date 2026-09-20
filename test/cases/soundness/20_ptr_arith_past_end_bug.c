// Pointer arithmetic past the end, then dereference.
// ASAN
int main(int argc, char **argv) {
  (void)argv;
  int a[4] = {0, 1, 2, 3};
  int *p = a;
  p += 3 + argc;
  return *p; // BUG: out-of-bounds // TRAP: span
}
