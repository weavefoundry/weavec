// Stack OOB write: while loop with <= bound.
// ASAN
int main(void) {
  char buf[8];
  int i = 0;
  while (i <= 8) {
    buf[i] = 0; // TRAP
    i++;
  }
  return buf[0];
}
