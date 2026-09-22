// CLEAN
// ASAN
int main(int argc, char **argv) {
  (void)argv;
  int a[4] = {0, 1, 2, 3};
  int *p = a;
  if (argc > 0 && argc < 4) p += argc;
  return *p;
}
