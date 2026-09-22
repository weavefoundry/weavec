// CLEAN
// ASAN
int main(void) {
  char buf[8];
  int i = 0;
  while (i < 8) {
    buf[i] = 0;
    i++;
  }
  return buf[0];
}
