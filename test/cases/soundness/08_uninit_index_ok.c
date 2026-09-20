// CLEAN
// ASAN
int main(int argc, char **argv) {
  (void)argv;
  int table[4] = {1, 2, 3, 4};
  int idx = 0;
  if (argc > 5) idx = 1;
  return table[idx];
}
