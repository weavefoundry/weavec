// Dangling pointer returned through an out-parameter.
// ASAN
static void get(int **out) {
  int local = 5;
  *out = &local;
}
int main(void) {
  int *p;
  get(&p);
  return *p; // TRAP
}
