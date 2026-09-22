// Correct: bounded arithmetic under a branch join.
// CLEAN
// ASAN
int main(int argc, char **argv) {
  (void)argv;
  int x = 0;
  if (argc & 1) x += 1; else x -= 1;
  if (argc & 2) x += 2; else x -= 2;
  if (argc & 4) x += 3; else x -= 3;
  int a[16] = {0};
  return a[x + 6];
}
