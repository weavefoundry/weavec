// Dangling pointer returned through an out-parameter.
// ASAN
static void get(int **out) {
  int local = 5;
  *out = &local; // BUG: lifetime-too-short
}
int main(void) {
  int *p;
  get(&p);
  return *p;
}
