// CLEAN
// ASAN
static int sum(const int *b, const int *e) {
  int s = 0;
  for (const int *p = b; p < e; ++p) s += *p;
  return s;
}
int main(void) {
  int a[4] = {1, 2, 3, 4};
  return sum(a, a + 4);
}
