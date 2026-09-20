// Off-by-one only reachable after many iterations.
// ASAN
int main(void) {
  int a[40];
  for (int i = 0; i < 40; i++) a[i] = i;
  int s = 0;
  for (int i = 0; i <= 40; i++) s += a[i]; // BUG: out-of-bounds
  return s;
}
