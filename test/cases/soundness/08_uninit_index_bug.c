// Uninitialized scalar used as array index.
int main(int argc, char **argv) {
  (void)argv;
  int table[4] = {1, 2, 3, 4};
  int idx;
  if (argc > 5) idx = 1;
  return table[idx]; // BUG: use-of-uninitialized // NEUTRALISED: zero-init
}
